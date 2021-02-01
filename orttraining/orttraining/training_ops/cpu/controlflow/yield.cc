// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/training_ops/cpu/controlflow/yield.h"
#include "orttraining/training_ops/cpu/controlflow/event_pool.h"
#include "orttraining/training_ops/cpu/controlflow/message_queue.h"
#include "core/framework/op_kernel_context_internal.h"

namespace onnxruntime {
namespace contrib {

ONNX_OPERATOR_KERNEL_EX(Yield, kMSDomain, 1, kCpuExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()), Yield);

ONNX_OPERATOR_KERNEL_EX(Hole, kMSDomain, 1, kCpuExecutionProvider,
                        KernelDefBuilder().TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()), Hole);

Status Yield::Compute(OpKernelContext* ctx) const {
  auto* ctx_internal = static_cast<OpKernelContextInternal*>(ctx);
  for (int i_in = 0; i_in < ctx->InputCount(); ++i_in) {
    onnxruntime::contrib::OrtMessageQueue::GetInstance().Push(*ctx_internal->GetInputMLValue(i_in));
  }

  // single event for InferenceSession::RunInBackgroundAndWaitForYield() that FW graph is done
  const int64_t main_thread_event_id = 0;
  OrtEventPool::GetInstance().SignalEvent(main_thread_event_id, OrtEventPool::TOKEN_YIELD_END_FORWARD);

  // wait for event from InferenceSession::ContinueRunInBackground() to continue the BW graph
  const int64_t background_thread_event_id = 1;
  OrtEventPool::GetInstance().ResetAndWaitEvent(background_thread_event_id);

  // Get output grad from somewhere and prepare Op outputs.
  for (int i_out = 0; i_out < ctx->OutputCount(); ++i_out) {
    ctx_internal->SetOutputMLValue(i_out, OrtMessageQueue::GetInstance().Pop());
  }

  return Status::OK();
}

// Hole executes when switching to Python for execution of a custom
// autograd function.  The implementation is very basic, just as a
// proof-of-concept for testing Megatron: we assume functions are
// single-input single-output, and we may need to do more to enforce
// ordering, and to manage inputs and outputs efficiently.

Status Hole::Compute(OpKernelContext* ctx) const {
  const OpKernelInfo& info = OpKernel::Info();
  int64_t external_fn_id;
  ORT_ENFORCE(info.GetAttr<int64_t>("external_fn", &external_fn_id).IsOK());
  int64_t is_backward;
  ORT_ENFORCE(info.GetAttr<int64_t>("is_backward", &is_backward).IsOK());

  // Pass data ORT->Python
  auto* ctx_internal = static_cast<OpKernelContextInternal*>(ctx);
  for (int i_in = 0; i_in < ctx->InputCount(); ++i_in) {
    OrtMessageQueue::GetInstance().Push(*ctx_internal->GetInputMLValue(i_in));
  }

  // Signal that a portion of the graph is complete
  const int64_t main_thread_event_id = 0;
  OrtEventPool::GetInstance().SignalEvent(main_thread_event_id,
                                          is_backward ? (OrtEventPool::TOKEN_HOLE_BACKWARD + external_fn_id)
                                          : (OrtEventPool::TOKEN_HOLE_FORWARD + external_fn_id));

  // Wait for resumption from Python
  const int64_t background_thread_event_id = 1;
  onnxruntime::contrib::OrtEventPool::GetInstance().ResetAndWaitEvent(background_thread_event_id);

  // Pass data Python->ORT
  for (int i_out = 0; i_out < ctx->OutputCount(); ++i_out) {
    ctx_internal->SetOutputMLValue(i_out, onnxruntime::contrib::OrtMessageQueue::GetInstance().Pop());
  }

  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime
