// Copyright(c) Microsoft Corporation.All rights reserved.
// Licensed under the MIT License.

#include <onnxruntime_c_api.h>
#include <onnxruntime_cxx_api.h>
#include "../include/onnxruntime/core/providers/cuda/cuda_provider_factory.h"
#include "multi_gpu_pipeline.h"
#include "task_thread_pool.h"
#include "Eigen/Core"
#include "Eigen/src/Core/arch/Default/Half.h"
#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/session/ort_apis.h"
#include <fstream>
#include <assert.h>

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 28020)
#endif
#include "single_include/nlohmann/json.hpp"
#ifdef _WIN32
#pragma warning(pop)
#endif

using json = nlohmann::json;

namespace onnxruntime {
namespace experimental {
// helper function to check for status
void CheckStatus(OrtStatus* status) {
  if (status != NULL) {
    const char* msg = OrtApis::GetErrorMessage(status);
    fprintf(stderr, "%s\n", msg);
    OrtApis::ReleaseStatus(status);
    exit(1);  // TODO throw an exception here
  }
}

struct Timer {
  using Clock = std::chrono::high_resolution_clock;
  Timer(const char* msg0) : msg(msg0), start(Clock::now()) {
  }
  ~Timer() {
    auto stop = Clock::now();
    std::cout << "TIMER: " << msg << " took " << std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count() << " microseconds\n";
  }
  const char* msg;
  std::chrono::time_point<Clock> start;
};

// returns a pair p; p.first is true if the elem is found in which case p.second is the index of the elem in the container
static std::pair<bool, int> Contains(const std::vector<std::string>& vec, const std::string& to_find) {
  auto it = std::find(std::begin(vec), std::end(vec), to_find);
  if (it != std::end(vec)) {
    return {true, it - std::begin(vec)};
  } else {
    return {false, -1};
  }
}

static std::vector<int64_t> GetShape(Ort::Session& sess,
                                     int io_idx,
                                     bool is_input) {
  std::vector<int64_t> retval;
  if (is_input) {
    retval = sess.GetInputTypeInfo(io_idx).GetTensorTypeAndShapeInfo().GetShape();
  } else {
    retval = sess.GetOutputTypeInfo(io_idx).GetTensorTypeAndShapeInfo().GetShape();
  }

  return retval;
}

RequestExecutionFrame::RequestExecutionFrame(PipelineSession& psess,  // passing by non-const exec_frame to create iobinding
                                             int req_idx0,
                                             ReqId req_id0,
                                             int batch_size0,
                                             int orig_input_seq_len0,
                                             int stage_id0,
                                             OrtResp& ort_resp0)
    : req_index(req_idx0),
      req_id(req_id0),
      batch_size(batch_size0),
      orig_input_seq_len(orig_input_seq_len0),
      stage_id(stage_id0),
      ort_resp(ort_resp0) {
  model_run_state_vec.reserve(psess.pcfg.num_stages);
  int idx = 0;
  for (const auto& mcfg : psess.pcfg.model_config_vec) {
    RunState rs;
    const auto& cuda_mem_info = psess.model_session_state_vec[idx].cuda_mem_info;
    auto& session = psess.model_session_state_vec[idx].session;
    auto cuda_allocator = std::make_unique<Ort::Allocator>(session, cuda_mem_info);

    // Pre-allocate memory for both present and past states
    // Calcuate the amount of memory to allocate
    // For now assume all present and past states have the same shape and the same indices for batch and seq dimension
    // This allows us to calculate the shape only once.
    auto rc = Contains(mcfg.input_names, mcfg.past_input_names[0]);
    auto io_idx = rc.second;
    auto past_present_state_shape = GetShape(session, io_idx, true);
    // override batch and seq dims with batch_size and maximum seq len
    past_present_state_shape[mcfg.batch_dim_index_in_state] = batch_size;
    past_present_state_shape[mcfg.seq_len_dim_index_in_state] = psess.pcfg.max_seq_len;
    auto num_elements = std::accumulate(std::begin(past_present_state_shape), std::end(past_present_state_shape), 1, std::multiplies<int>());
    int size_to_allocate = sizeof(Ort::Float16_t) * num_elements;  // TODO don't hardcode type

    // pre-allocate buffers for input and output states
    for (int i = 0, end = mcfg.past_input_names.size(); i < end; ++i) {
      rs.present_past_prealloc_buffer_1_vec.push_back(cuda_allocator->GetAllocation(size_to_allocate));
      rs.present_past_prealloc_buffer_2_vec.push_back(cuda_allocator->GetAllocation(size_to_allocate));
    }

    // initialize the output states
    // intentionally 0 since when the model is run the first time, there's no past state to feed.
    past_present_state_shape[mcfg.seq_len_dim_index_in_state] = 0;
    for (int j = 0, end = mcfg.present_output_names.size(); j < end; ++j) {
      const auto& oname = mcfg.present_output_names[j];
      auto& mem_allocation = rs.present_past_prealloc_buffer_1_vec[j];  // careful, use buffer1 here
      auto ort_val = Ort::Value::CreateTensor(
          cuda_mem_info, mem_allocation.get(), mem_allocation.size(),
          past_present_state_shape.data(), past_present_state_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);  // TODO remove hardcoded type
      rs.output_val_map[oname] = OrtValueHandle(ort_val.release());
    }

    // it's inefficient to allocate memory for the inter stage outputs for every step
    // pre-allocate buffers for inter stage outputs except the last stage
    if (idx < psess.pcfg.num_stages - 1) {
      for (const auto& elem_pair : mcfg.inter_stage_output_input_map) {
        // get the shape of the output name
        const auto& oname = elem_pair.first;
        auto rc = Contains(mcfg.output_names, oname);
        auto output_shape = GetShape(session, rc.second, false /*output*/);

        // replace seq_len dim with max_seq_len
        output_shape[mcfg.batch_dim_in_inter_stage_output] = batch_size;
        output_shape[mcfg.seq_len_dim_in_inter_stage_output] = psess.pcfg.max_seq_len;

        // get the total number of bytes to allocate
        auto num_elements = std::accumulate(std::begin(output_shape), std::end(output_shape), 1, std::multiplies<int>());
        int size_to_allocate = sizeof(Ort::Float16_t) * num_elements;  // TODO don't hardcode type
        // std::cout << "inter stage output num_elements " << num_elements << "\n";
        // allocate and store in map
        rs.inter_stage_output_prealloc_buffer_map.emplace(oname, cuda_allocator->GetAllocation(size_to_allocate));
      }
    }

    rs.io_binding = std::make_unique<Ort::IoBinding>(psess.model_session_state_vec[idx].session);
    rs.cuda_allocator = std::move(cuda_allocator);
    model_run_state_vec.push_back(std::move(rs));

    ++idx;
  }
}

static ReqId CreateRequestId() {
  static int req_id;
  return ++req_id;
  // using namespace std::chrono;
  // return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

static float HalfToFloat(uint16_t h) {
  return Eigen::half_impl::half_to_float(Eigen::half_impl::raw_uint16_to_half(h));
}

static Token* ExecuteRequest(Token& token,
                             const PipelineConfig::ModelConfig& mcfg,
                             PipelineSession::SessionState& session_state,
                             RequestExecutionFrame& exec_frame /* pass by non-const exec_frame intentional as we'll update the state */) {
  // std::ostringstream ostr;
  // ostr << "Executing req_id(" << token.req_id << ")/step(" << token.step_id << ")/stage(" << exec_frame.stage_id << ")";
  // const std::string& str = ostr.str();
  // std::cout << str << "\n";
  // Timer t(str.c_str());

  int model_idx = exec_frame.stage_id;
  RequestExecutionFrame::RunState& run_state = exec_frame.model_run_state_vec[model_idx];

  // set the GPU device id for this thread
  CheckStatus(OrtApis::SetCurrentGpuDeviceId(mcfg.device_id));

  // reuse the token; move the things out of this token since we'll overwrite them
  auto* out_token_ptr = &token;
  auto in_token_ort_value_names = token.ort_value_names;
  std::vector<OrtValueHandle> in_token_ort_values;
  in_token_ort_values.reserve(token.ort_values.size());
  for (auto& v : token.ort_values) {
    in_token_ort_values.push_back(std::move(v));
  }
  token.ort_value_names.clear();
  token.ort_values.clear();

  // if (token.step_id > 0 && exec_frame.stage_id == 0) {
  //   auto* posn_ids_data = in_token_ort_values[1].GetTensorData<int64_t>();
  //   for (int i = 0; i < exec_frame.batch_size; ++i) {
  //     std::cout << "step_id: " << token.step_id << " posn: " << posn_ids_data[i] << "\n";
  //   }
  // }

  auto& io_binding_obj = *run_state.io_binding;
  auto* io_binding = static_cast<OrtIoBinding*>(io_binding_obj);
  io_binding_obj.ClearBoundInputs();
  io_binding_obj.ClearBoundOutputs();

  // inputs
  // go through all the inputs from the config and for each one if you find it in token.input_names
  // use the value from there.
  // else search this input name inside past_input_names. If found, get the corresponding output name from
  // present_output_names and the OrtValue associated with it.
  for (const auto& iname : mcfg.input_names) {
    auto rc = Contains(in_token_ort_value_names, iname);
    if (rc.first) {
      // std::cout << stage_id << "/" << token.step_id << " binding input " << token.ort_value_names[rc.second] << "\n";
      CheckStatus(OrtApis::BindInput(io_binding, iname.c_str(), in_token_ort_values[rc.second]));
      continue;
    }

    rc = Contains(mcfg.past_input_names, iname);
    if (rc.first) {
      const auto& mapped_oname = mcfg.present_output_names[rc.second];
      // std::cout << stage_id << "/" << token.step_id << " state_binding " << iname << " with value of " << mapped_oname << "\n";
      CheckStatus(OrtApis::BindInput(io_binding, iname.c_str(), run_state.output_val_map.at(mapped_oname)));
    }
  }

  // allocate outputs
  // output seq len = current input seq len + past seq len (which is 0 the first time)
  // if output is a state, use the pre-allocated buffer to create an OrtValue and bind it.
  // if output is not a state, bind using just cuda_mem_info.

  // get seq len of input_ids (stage 0) or input_hidden_states (stage 1)
  auto rc = Contains(in_token_ort_value_names, mcfg.input_to_use_for_seq_len);
  ORT_ENFORCE(rc.first, mcfg.input_to_use_for_seq_len, " not present in Token.");
  const auto& input_ort_value = in_token_ort_values[rc.second];
  int input_seq_len = input_ort_value.GetTensorTypeAndShapeInfo().GetShape()[mcfg.seq_len_dim_index_in_input];

  // get past seq len
  // assume past_seq_len is same for all states
  int past_seq_len = run_state.output_val_map.at(mcfg.present_output_names[0])
                         .GetTensorTypeAndShapeInfo()
                         .GetShape()[mcfg.seq_len_dim_index_in_state];
  // std::cout << "input_seq_len: " << input_seq_len << "\n";
  // std::cout << "past_seq_len: " << past_seq_len << "\n";
  // new seq len for state output = seq len of input_ids + past_seq_len
  int new_seq_len = input_seq_len + past_seq_len;

  auto& ort_sess = session_state.session;

  // populate shape for state outputs
  // assume same shape for all outputs
  auto rc2 = Contains(mcfg.output_names, mcfg.present_output_names[0]);
  auto out_idx = rc2.second;
  auto past_present_state_shape = GetShape(ort_sess, out_idx, false /*output*/);
  past_present_state_shape[mcfg.batch_dim_index_in_state] = exec_frame.batch_size;
  past_present_state_shape[mcfg.seq_len_dim_index_in_state] = new_seq_len;

  // assume types are same for all states
  auto past_present_type = ort_sess.GetOutputTypeInfo(out_idx).GetTensorTypeAndShapeInfo().GetElementType();

  for (int oidx = 0, end = mcfg.output_names.size(); oidx < end; ++oidx) {
    const auto& oname = mcfg.output_names[oidx];
    auto rc = Contains(mcfg.present_output_names, oname);
    if (rc.first) {
      auto& mem_allocation = token.step_id % 2 == 0  // even: use buffer1 for input and buffer2 for output
                                 ? run_state.present_past_prealloc_buffer_2_vec[rc.second]
                                 : run_state.present_past_prealloc_buffer_1_vec[rc.second];
      // std::cout << "mem allocation size: " << mem_allocation.size() << "\n";
      auto output_ort_val = Ort::Value::CreateTensor(
          session_state.cuda_mem_info, mem_allocation.get(), mem_allocation.size(),
          past_present_state_shape.data(), past_present_state_shape.size(), past_present_type);
      // std::cout << "step(" << token.step_id << ") / stage(" << exec_frame.stage_id << ")"
      //           << " created tensor for " << oname << "\n";
      CheckStatus(OrtApis::BindOutput(io_binding, oname.c_str(), output_ort_val));
    } else {
      // if oname is present in OrtResp::output_names, bind the corresponding OrtValue from OrtResp
      // we check in OrtResp::output_names because we want to use the info provided by the user to tell us
      // where the output should go.
      auto rc = Contains(exec_frame.ort_resp.output_names, oname);
      if (rc.first) {  // logits
        // get the corresponding ortval from OrtResp
        auto* mem_info = exec_frame.ort_resp.output_meminfo[rc.second];
        // if user provided mem_info, use that for binding
        if (mem_info) {
          CheckStatus(OrtApis::BindOutputToDevice(io_binding, oname.c_str(), mem_info));
        } else {  // bind the pre-allocated OrtVal
          const auto& ort_val = exec_frame.ort_resp.output_values[rc.second];
          CheckStatus(OrtApis::BindOutput(io_binding, oname.c_str(), ort_val));
        }
      } else {  // inter stage outputs (e.g. hidden_states)
        // get shape of oname
        auto inter_stage_output_shape = GetShape(ort_sess, oidx, false /*output*/);

        // replace batch_size and seq_len
        inter_stage_output_shape[mcfg.batch_dim_in_inter_stage_output] = exec_frame.batch_size;
        inter_stage_output_shape[mcfg.seq_len_dim_in_inter_stage_output] = input_seq_len;

        auto& mem_allocation = run_state.inter_stage_output_prealloc_buffer_map.at(oname);
        auto inter_stage_ort_val = Ort::Value::CreateTensor(
            session_state.cuda_mem_info, mem_allocation.get(), mem_allocation.size(),
            inter_stage_output_shape.data(), inter_stage_output_shape.size(), past_present_type);
        CheckStatus(OrtApis::BindOutput(io_binding, oname.c_str(), inter_stage_ort_val));
      }
    }
  }

  // run
  // std::cout << "step(" << token.step_id << ") / stage(" << exec_frame.stage_id << ")"
  //           << " just before run\n";
  {
    // std::string run_timer_str = "Run: " + str;
    // Timer t2(run_timer_str.c_str());
    ort_sess.Run({}, io_binding_obj);
  }
  // std::cout << "step(" << token.step_id << ") / stage(" << exec_frame.stage_id << ")"
  //           << " Done with run\n";
  // now populate token and save state from this run
  auto vec_out_vals = io_binding_obj.GetOutputValues();
  for (int i = 0, end = mcfg.output_names.size(); i < end; ++i) {
    const auto& oname = mcfg.output_names[i];

    // Assume that the same output name is not present in both the state that needs to be kept
    // and that needs to be passed on to the next layer.
    auto is_loop_back_state_output = Contains(mcfg.present_output_names, oname);
    assert(!(is_loop_back_state_output.first && mcfg.inter_stage_output_input_map.count(oname)));

    // if this output is present in present_output_names, store it in model_run_state_vec
    // because we don't want to store all outputs
    if (is_loop_back_state_output.first) {
      // std::cout << "step(" << token.step_id << ") / stage(" << exec_frame.stage_id << ")"
      //           << " saving state " << oname << "\n";
      // assert(vec_out_vals[i].GetTensorData<Ort::Float16_t>());
      run_state.output_val_map[oname] = OrtValueHandle(vec_out_vals[i].release());
      continue;
    }

    // only pass those outputs to the next layer for which there is a config in the ensemble
    // other outputs are states to be used in the next run
    if (mcfg.inter_stage_output_input_map.count(oname)) {
      // std::cout << "Copying output req_id(" << token.req_id << ")/step(" << token.step_id << ")/stage(" << exec_frame.stage_id << ") "
      // << mcfg.inter_stage_output_input_map.at(oname) << "\n";
      out_token_ptr->ort_value_names.push_back(mcfg.inter_stage_output_input_map.at(oname));  // input_hidden_states
      // assert(vec_out_vals[i].GetTensorData<Ort::Float16_t>());
      // if (oname == "hidden_states") {
      //   auto* data = vec_out_vals[i].GetTensorData<Ort::Float16_t>();
      //   auto typeshape = vec_out_vals[i].GetTensorTypeAndShapeInfo();
      //   auto slen = typeshape.GetShape()[1];
      //   // std::cout << "slen: " << slen << " bsize: " << typeshape.GetShape()[0] << "\n";
      //   std::vector<Ort::Float16_t> v(exec_frame.batch_size * slen * 4096);
      //   cudaMemcpy(v.data(), data, sizeof(Ort::Float16_t) * exec_frame.batch_size * slen * 4096, cudaMemcpyDefault);
      //   for (int b = 0; b < exec_frame.batch_size * 94 * 4096; b += 94 * 4096) {
      //     std::cout << "b = " << b << "\n";
      //     for (int k = 0; k < 10; ++k) {
      //       printf("output hidden state %f\n", HalfToFloat(v[b + k]));
      //     }
      //   }
      // }
      out_token_ptr->ort_values.push_back(OrtValueHandle(vec_out_vals[i].release()));
    }
  }

  // std::cout << "Done executing req_id(" << token.req_id << ")/step(" << token.step_id << ")/stage(" << exec_frame.stage_id << ")"
  //           << "\n";
  return out_token_ptr;
};

static void GetNewInputIdsFromLogits(int batch_size,
                                     const OrtValueHandle& logits,
                                     const std::vector<int64_t>& logits_shape,
                                     int eos_token,
                                     std::vector<int64_t>& input_ids,
                                     std::vector<int64_t>& input_ids_shape,
                                     bool& are_all_input_ids_eos_tokens) {
  // Timer t("GetNewInputIdsFromLogits");
  are_all_input_ids_eos_tokens = false;
  input_ids.clear();
  input_ids_shape.clear();

  input_ids.reserve(batch_size);
  input_ids_shape = std::vector<int64_t>{batch_size, 1};
  const auto* logits_data = logits.GetTensorData<Ort::Float16_t>();

  // for (int x = 0; x < 10; ++x) {
  //   printf("logits uint %hu, float val: %f\n", logits_data[x].value, HalfToFloat(logits_data[x]));
  // }

  int num_elems = logits_shape[0] * logits_shape[1] * logits_shape[2];
  int ltwo = logits_shape[1] * logits_shape[2];
  int skip = (logits_shape[1] - 1) * logits_shape[2];

  // now find the max per onnx batch
  int num_eos_tokens_predicted = 0;
  for (int batch_id = 0; batch_id < num_elems; batch_id += ltwo) {  // TODO parallelize on batches
    // std::cout << "batch_id " << batch_id << " first: " << logits_data[batch_id + skip] << "\n";
    auto tmp = std::max_element(logits_data + batch_id + skip,
                                logits_data + batch_id + skip + logits_shape[2],
                                [](const Ort::Float16_t& a, const Ort::Float16_t& b) { return HalfToFloat(a) < HalfToFloat(b); });
    int64_t max_idx = std::distance(logits_data + batch_id + skip, tmp);
    // if (batch_id == 0)
    // std::cout << "batch_id: " << batch_id << " next token: " << max_idx << "\n";
    if (max_idx == eos_token) {
      ++num_eos_tokens_predicted;
    }
    input_ids.push_back(max_idx);
  }
  are_all_input_ids_eos_tokens = (num_eos_tokens_predicted == batch_size);
}

void GetNewPosnIds(int batch_size, int orig_input_seq_len, int step_id, std::vector<int64_t>& posn_ids) {
  int new_posn_id = orig_input_seq_len + step_id - 1;
  // std::cout << "new posn id: " << new_posn_id << "\n";
  posn_ids.assign(batch_size, new_posn_id);
}

OrtStatus* PipelineSession::HandleAndReturnFailure(const char* error_msg) {
  for (auto& stage : pipeline_stages) {
    stage->DrainAllInflightRequests();
  }
  return OrtApis::CreateStatus(ORT_FAIL, error_msg);
}

OrtStatus* PipelineSession::CopyFinalOutput(Token& token, OrtResp& ort_resp) {
  int resp_index = 0;
  for (const auto& oname : ort_resp.output_names) {
    auto ex = Contains(token.ort_value_names, oname);
    if (ex.first) {
      ort_resp.output_values[resp_index] = token.ort_values[ex.second].release();
    } else {
      // case when the user requested output was not present in the final output
      std::ostringstream ostr;
      ostr << "Error: Output " << oname << " is not produced by the final stage\n";
      return HandleAndReturnFailure(ostr.str().c_str());
    }
    ++resp_index;
  }
  return nullptr;
}

void PipelineSession::ThreadWorkerFn(ResponseQueue& resp_queue,
                                     Token& token,
                                     const PipelineConfig::ModelConfig& mcfg,
                                     PipelineSession::SessionState& session_state,
                                     RequestExecutionFrame& exec_frame) {
  Token* out_token_ptr = nullptr;
  try {
    out_token_ptr = ExecuteRequest(token, mcfg, session_state, exec_frame);
  } catch (const std::exception& e) {
    std::ostringstream error;
    error << "Error in processing request id: " << token.req_id << " with exception: " << e.what();
    out_token_ptr = &exec_frame.token;
    out_token_ptr->req_id = token.req_id;
    out_token_ptr->step_id = token.step_id;
    out_token_ptr->error_msg = error.str();
  } catch (...) {
    std::ostringstream error;
    error << "Error in processing request id: " << token.req_id << " with unknown exception";
    out_token_ptr = &exec_frame.token;
    out_token_ptr->req_id = token.req_id;
    out_token_ptr->step_id = token.step_id;
    out_token_ptr->error_msg = error.str();
  }
  resp_queue.Push(out_token_ptr);
}

void PipelineSession::SetupAndScheduleAllRequestsToStage0(const std::vector<OrtReq>& req_list,
                                                          std::vector<OrtResp>& resp_list,
                                                          std::unordered_map<ReqId, RequestExecutionFrame>& req_frame_map,
                                                          ResponseQueue& resp_queue) {
  for (int req_idx = 0, num_reqs = req_list.size(); req_idx < num_reqs; ++req_idx) {
    ReqId req_id = CreateRequestId();
    // std::cout << "creating req_id: " << req_id << "\n";
    auto& one_req = req_list[req_idx];
    auto& one_resp = resp_list[req_idx];

    // store batch size and input seq len to change position_ids for step > 0
    auto rc = Contains(one_req.input_names, pcfg.model_config_vec[0].input_to_use_for_seq_len);
    std::vector<OrtValueHandle> i_values;
    i_values.reserve(one_req.input_values.size());
    for (auto& v : one_req.input_values) {
      i_values.push_back(OrtValueHandle(v, false));  // don't own the OrtValues supplied by the user
    }
    const auto& shape = i_values[rc.second]
                            .GetTensorTypeAndShapeInfo()
                            .GetShape();
    int orig_seq_len = shape[pcfg.model_config_vec[0].seq_len_dim_index_in_input];
    int batch_size = shape[pcfg.model_config_vec[0].batch_dim_index_in_input];

    // create and store RequestExecutionFrame
    int stage_id = 0;
    RequestExecutionFrame tmp_exec_frame(*this, req_idx, req_id, batch_size, orig_seq_len, stage_id, one_resp);
    req_frame_map.emplace(req_id, std::move(tmp_exec_frame));

    // schedule request
    int step_id = 0;
    auto* in_token_ptr = &req_frame_map.at(req_id).token;
    in_token_ptr->Init(req_id, step_id, one_req.input_names, std::move(i_values));
    auto& exec_frame = req_frame_map.at(req_id);
    const auto& model_config = pcfg.model_config_vec[stage_id];
    auto& session_state = model_session_state_vec[stage_id];
    auto lambda = [this, &model_config, &session_state, &resp_queue, in_token_ptr, &exec_frame]() {
      this->ThreadWorkerFn(resp_queue, *in_token_ptr, model_config, session_state, exec_frame);
    };
    std::function<void()> task(lambda);

    // schedule the request to the first stage
    pipeline_stages[0]->ScheduleTask(std::move(task));
  }
}

OrtStatus* PipelineSession::ProcessResponses(int num_reqs,
                                             int num_steps,
                                             std::unordered_map<ReqId, RequestExecutionFrame>& req_frame_map,
                                             ResponseQueue& resp_queue,
                                             std::vector<OrtResp>& resp_list) {
  // now read the response queue and enqueue further steps/stages for processing
  // passing the output of one stage to the next one
  auto cpu_memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  int req_processed = 0;
  while (req_processed < num_reqs) {
    Token* token_ptr = nullptr;
    auto rc = resp_queue.WaitAndPop(10000 /*TODO make it configurable*/, token_ptr);
    if (rc != ResponseQueue::Status::kSuccess) {
      return HandleAndReturnFailure("Request processing timed out after 1000 ms");
    }
    ReqId req_id = token_ptr->req_id;
    int step_id = token_ptr->step_id;
    auto& exec_frame = req_frame_map.at(req_id);

    // fail the whole batch if even one req fails
    if (!token_ptr->error_msg.empty()) {
      return HandleAndReturnFailure(token_ptr->error_msg.c_str());
    }

    exec_frame.stage_id = (exec_frame.stage_id + 1) % pcfg.num_stages;
    if (exec_frame.stage_id == 0) {  // this means we've reached step > 0
      ++step_id;
      if (step_id == num_steps) {  // we're done with all steps of this request, move the output
        // look for the requested output_names in the token
        // fetch the corresponding Ort::Value and copy it to OrtResp
        auto status = CopyFinalOutput(*token_ptr, resp_list[exec_frame.req_index]);
        if (status) {
          return status;
        }
        req_frame_map.erase(req_id);
        ++req_processed;
        continue;
      } else {  // done with one step; now start the next step for this request
        int batch_size = exec_frame.batch_size;

        // update input_ids
        // get index of 'logits' output
        auto rc = Contains(token_ptr->ort_value_names, pcfg.logits_name);
        if (!rc.first) {
          return HandleAndReturnFailure("Did not get logits in the output");
        }
        auto& input_ids = exec_frame.next_step_input_buffer_map[pcfg.input_ids_name].data;
        auto& input_ids_shape = exec_frame.next_step_input_buffer_map[pcfg.input_ids_name].shape;
        const auto& logits_ort_val = token_ptr->ort_values[rc.second];
        auto logits_shape = logits_ort_val.GetTensorTypeAndShapeInfo().GetShape();
        bool are_all_input_ids_eos_tokens = false;
        GetNewInputIdsFromLogits(batch_size, logits_ort_val, logits_shape, pcfg.eos_token,
                                 input_ids, input_ids_shape,
                                 are_all_input_ids_eos_tokens);
        // If the next set of generated input ids are EOS tokens, it's time to stop this request
        // even if we've not reached num_steps
        if (are_all_input_ids_eos_tokens) {
          std::cout << "All input ids are EOS tokens; aborting this request\n";
          auto status = CopyFinalOutput(*token_ptr, resp_list[exec_frame.req_index]);
          if (status) {
            return status;
          }
          req_frame_map.erase(req_id);
          ++req_processed;
          continue;
        }

        // assume shape is same for both input_ids and position_ids
        auto input_ids_tensor = Ort::Value::CreateTensor<int64_t>(cpu_memory_info, input_ids.data(), input_ids.size(),
                                                                  input_ids_shape.data(), input_ids_shape.size());  // TODO don't hardcode type

        // update position ids
        // assume shape of position ids is same as input_ids
        auto& posn_ids = exec_frame.next_step_input_buffer_map[pcfg.logits_name].data;
        GetNewPosnIds(batch_size, exec_frame.orig_input_seq_len, step_id, posn_ids);

        auto posn_ids_tensor = Ort::Value::CreateTensor<int64_t>(cpu_memory_info, posn_ids.data(), posn_ids.size(),
                                                                 input_ids_shape.data(), input_ids_shape.size());  // TODO don't hardcode type

        // clear and fill Token for the next step for this request
        token_ptr->Clear();
        token_ptr->req_id = req_id;
        token_ptr->step_id = step_id;
        token_ptr->ort_value_names = {pcfg.input_ids_name, pcfg.position_ids_name};
        token_ptr->ort_values.push_back(OrtValueHandle(input_ids_tensor.release()));
        token_ptr->ort_values.push_back(OrtValueHandle(posn_ids_tensor.release()));
      }
    } else {
      // continue executing the next stage; the outputs that need to be passed on to the next stage
      // are already present in the token we got from the resp queue
      token_ptr->req_id = req_id;
      token_ptr->step_id = step_id;
    }

    // re-enqueue request
    const auto& model_config = pcfg.model_config_vec[exec_frame.stage_id];
    auto& session_state = model_session_state_vec[exec_frame.stage_id];
    auto lambda = [this, &model_config, &session_state, &resp_queue, token_ptr, &exec_frame]() {
      this->ThreadWorkerFn(resp_queue, *token_ptr, model_config, session_state, exec_frame);
    };
    std::function<void()> task(lambda);

    // enqueue the request to the correct stage
    pipeline_stages[exec_frame.stage_id]->ScheduleTask(std::move(task));
  }

  return nullptr;
}

static OrtStatus* ValidateRequest(const std::vector<OrtReq>& req_list, const std::vector<OrtResp>& resp_list) {
  if (req_list.size() != resp_list.size()) {
    return OrtApis::CreateStatus(ORT_INVALID_ARGUMENT, "Size of request and response lists differ.");
  }

  for (int i = 0, end = req_list.size(); i < end; ++i) {
    const auto& ivalues = req_list[i].input_names;
    const auto& inames = req_list[i].input_values;
    if (ivalues.size() != inames.size()) {
      std::ostringstream ostr;
      ostr << "Size of request names and OrtValues differ for index " << i;
      return OrtApis::CreateStatus(ORT_INVALID_ARGUMENT, ostr.str().c_str());
    }

    const auto& ovalues = resp_list[i].output_values;
    const auto& onames = resp_list[i].output_names;
    if (ovalues.size() != onames.size()) {
      std::ostringstream ostr;
      ostr << "Size of response names and OrtValues differ for index " << i;
      return OrtApis::CreateStatus(ORT_INVALID_ARGUMENT, ostr.str().c_str());
    }
  }

  return nullptr;
}

// TODO proper error handling
// TODO - replace all cout with LOG
// For simplicity even if one req in the batch fails, we consider the full batch to have failed.
OrtStatus* PipelineSession::Run(const std::vector<OrtReq>& req_list, std::vector<OrtResp>& resp_list, int num_steps) {
  auto status = ValidateRequest(req_list, resp_list);
  if (status) {
    return status;
  }

  ResponseQueue resp_queue;
  std::unordered_map<ReqId, RequestExecutionFrame> req_frame_map;

  SetupAndScheduleAllRequestsToStage0(req_list, resp_list, req_frame_map, resp_queue);

  int num_reqs = req_list.size();
  status = ProcessResponses(num_reqs, num_steps, req_frame_map, resp_queue, resp_list);
  return status;
}

void PipelineSession::ParseEnsembleFile(const std::string& ensemble_config_file_path, PipelineConfig& pcfg) {
  std::ifstream ifs(ensemble_config_file_path);
  if (!ifs.good()) {
    throw std::runtime_error(std::string("Error reading file ") + ensemble_config_file_path);
  }

  auto j = json::parse(ifs, nullptr, true);

  pcfg.eos_token = j["eos_token"];
  pcfg.input_ids_name = j["input_ids_name"];
  pcfg.position_ids_name = j["position_ids_name"];
  pcfg.logits_name = j["logits_name"];
  pcfg.max_seq_len = j["max_seq_len"];
  int idx = 0;
  for (const auto& m : j["ensemble"]) {
    PipelineConfig::ModelConfig cfg;
    std::string model_name = m["model_name"];
    cfg.model_name = model_name;
    cfg.model_file_path = m["model_file_path"];
    cfg.input_to_use_for_seq_len = m["input_to_use_for_seq_len"];
    cfg.seq_len_dim_index_in_input = m["seq_len_dim_index_in_input"];
    cfg.batch_dim_index_in_input = m["batch_dim_index_in_input"];
    cfg.batch_dim_index_in_state = m["batch_dim_index_in_state"];
    cfg.seq_len_dim_index_in_state = m["seq_len_dim_index_in_state"];
    cfg.seq_len_dim_in_inter_stage_output = m["seq_len_dim_in_inter_stage_output"];
    cfg.batch_dim_in_inter_stage_output = m["batch_dim_in_inter_stage_output"];
    cfg.device_id = m["device_id"];

    const char* key = "inter_stage_output_input_map";
    if (m.find(key) != m.end()) {
      const auto& j_oi_map = m[key];
      for (const auto& elem : j_oi_map) {
        cfg.inter_stage_output_input_map[elem[0]] = elem[1];
      }
    }

    key = "past_input_names";
    if (m.find(key) != m.end()) {
      const auto& si_names = m[key];
      for (const auto& elem : si_names) {
        cfg.past_input_names.push_back(elem);
      }
    }

    key = "present_output_names";
    if (m.find(key) != m.end()) {
      const auto& so_names = m[key];
      for (const auto& elem : so_names) {
        cfg.present_output_names.push_back(elem);
      }
    }

    pcfg.model_config_vec.push_back(std::move(cfg));
    pcfg.model_idx_map[model_name] = idx;
    ++idx;
  }

  pcfg.num_stages = pcfg.model_config_vec.size();
}

bool PipelineSession::Validate(const PipelineConfig& pcfg) {
  ORT_UNUSED_PARAMETER(pcfg);
  // TODO validate
  return true;
}

PipelineSession::PipelineSession(const std::string& ensemble_config_file_path, const OrtEnv& env) {
  ParseEnsembleFile(ensemble_config_file_path, pcfg);
  auto rc = Validate(pcfg);
  ORT_ENFORCE(rc, "Validation of the config file failed.");
  Init(pcfg, env);
}

PipelineSession::PipelineSession(const PipelineConfig& ens0, const OrtEnv& env) : pcfg(ens0) {
  auto rc = Validate(pcfg);
  ORT_ENFORCE(rc, "Validation of the config file failed.");
  Init(pcfg, env);
}

void PipelineSession::Init(PipelineConfig& pcfg, const OrtEnv& env) {
  Ort::AllocatorWithDefaultOptions ort_allocator;
  pipeline_stages.reserve(pcfg.model_config_vec.size());

  for (auto& mcfg : pcfg.model_config_vec) {
    Ort::SessionOptions session_options;
    session_options.DisablePerSessionThreads();
    CheckStatus(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, mcfg.device_id));
    Ort::Session session{nullptr};
    {
      std::string session_time_msg = mcfg.model_name;
      session_time_msg.append(" session creation");
      Timer t(session_time_msg.c_str());
      OrtSession* ort_sess;
      CheckStatus(OrtApis::CreateSession(&env, mcfg.model_file_path.c_str(), session_options, &ort_sess));
      session = Ort::Session(ort_sess);
    }

    // fill output names
    int output_count = session.GetOutputCount();
    mcfg.output_names.reserve(output_count);
    for (int i = 0; i < output_count; ++i) {
      auto name_ptr = std::unique_ptr<char>(session.GetOutputName(i, ort_allocator));
      mcfg.output_names.push_back(std::string(name_ptr.get()));
    }

    // fill input names
    int input_count = session.GetInputCount();
    mcfg.input_names.reserve(input_count);
    for (int i = 0; i < input_count; ++i) {
      auto name_ptr = std::unique_ptr<char>(session.GetInputName(i, ort_allocator));
      mcfg.input_names.push_back(std::string(name_ptr.get()));
    }

    // create session state
    Ort::MemoryInfo cuda_mem_info("Cuda", OrtDeviceAllocator, mcfg.device_id, OrtMemTypeDefault);
    SessionState sess_state{std::move(session), std::move(cuda_mem_info)};
    model_session_state_vec.push_back(std::move(sess_state));

    // create stages
    pipeline_stages.push_back(std::make_unique<PipelineStage>(mcfg.device_id, 1 /*thread pool size per stage*/));
  }
}
}  // namespace experimental
}  // namespace onnxruntime