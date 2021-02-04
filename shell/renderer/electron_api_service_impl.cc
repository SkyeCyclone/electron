// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "electron/shell/renderer/electron_api_service_impl.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/macros.h"
#include "base/threading/thread_restrictions.h"
#include "gin/data_object_builder.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "shell/common/electron_constants.h"
#include "shell/common/gin_converters/blink_converter.h"
#include "shell/common/gin_converters/value_converter.h"
#include "shell/common/heap_snapshot.h"
#include "shell/common/node_includes.h"
#include "shell/common/options_switches.h"
#include "shell/common/v8_value_serializer.h"
#include "shell/renderer/electron_render_frame_observer.h"
#include "shell/renderer/renderer_client_base.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom-shared.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_message_port_converter.h"

namespace electron {

namespace {

const char kIpcKey[] = "ipcNative";

// Gets the private object under kIpcKey
v8::Local<v8::Object> GetIpcObject(v8::Local<v8::Context> context) {
  auto* isolate = context->GetIsolate();
  auto binding_key = gin::StringToV8(isolate, kIpcKey);
  auto private_binding_key = v8::Private::ForApi(isolate, binding_key);
  auto global_object = context->Global();
  auto value =
      global_object->GetPrivate(context, private_binding_key).ToLocalChecked();
  if (value.IsEmpty() || !value->IsObject()) {
    LOG(ERROR) << "Attempted to get the 'ipcNative' object but it was missing";
    return v8::Local<v8::Object>();
  }
  return value->ToObject(context).ToLocalChecked();
}

void InvokeIpcCallback(v8::Local<v8::Context> context,
                       const std::string& callback_name,
                       std::vector<v8::Local<v8::Value>> args) {
  TRACE_EVENT0("devtools.timeline", "FunctionCall");
  auto* isolate = context->GetIsolate();

  auto ipcNative = GetIpcObject(context);
  if (ipcNative.IsEmpty())
    return;

  // Only set up the node::CallbackScope if there's a node environment.
  // Sandboxed renderers don't have a node environment.
  node::Environment* env = node::Environment::GetCurrent(context);
  std::unique_ptr<node::CallbackScope> callback_scope;
  if (env) {
    callback_scope.reset(new node::CallbackScope(isolate, ipcNative, {0, 0}));
  }

  auto callback_key = gin::ConvertToV8(isolate, callback_name)
                          ->ToString(context)
                          .ToLocalChecked();
  auto callback_value = ipcNative->Get(context, callback_key).ToLocalChecked();
  DCHECK(callback_value->IsFunction());  // set by init.ts
  auto callback = callback_value.As<v8::Function>();
  ignore_result(callback->Call(context, ipcNative, args.size(), args.data()));
}

void EmitIPCEvent(v8::Local<v8::Context> context,
                  bool internal,
                  const std::string& channel,
                  std::vector<v8::Local<v8::Value>> ports,
                  v8::Local<v8::Value> args,
                  int32_t sender_id) {
  auto* isolate = context->GetIsolate();

  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  v8::MicrotasksScope script_scope(isolate,
                                   v8::MicrotasksScope::kRunMicrotasks);

  std::vector<v8::Local<v8::Value>> argv = {
      gin::ConvertToV8(isolate, internal), gin::ConvertToV8(isolate, channel),
      gin::ConvertToV8(isolate, ports), args,
      gin::ConvertToV8(isolate, sender_id)};

  InvokeIpcCallback(context, "onMessage", argv);
}

}  // namespace

ElectronApiServiceImpl::~ElectronApiServiceImpl() = default;

ElectronApiServiceImpl::ElectronApiServiceImpl(
    content::RenderFrame* render_frame,
    RendererClientBase* renderer_client)
    : content::RenderFrameObserver(render_frame),
      renderer_client_(renderer_client) {}

void ElectronApiServiceImpl::BindTo(
    mojo::PendingAssociatedReceiver<mojom::ElectronRenderer> receiver) {
  if (document_created_) {
    if (receiver_.is_bound())
      receiver_.reset();

    receiver_.Bind(std::move(receiver));
    receiver_.set_disconnect_handler(base::BindOnce(
        &ElectronApiServiceImpl::OnConnectionError, GetWeakPtr()));
  } else {
    pending_receiver_ = std::move(receiver);
  }
}

void ElectronApiServiceImpl::ProcessPendingMessages() {
  while (!pending_messages_.empty()) {
    auto msg = std::move(pending_messages_.front());
    pending_messages_.pop();

    Message(msg.internal, msg.channel, std::move(msg.arguments), msg.sender_id);
  }
}

void ElectronApiServiceImpl::DidCreateDocumentElement() {
  document_created_ = true;

  if (pending_receiver_) {
    if (receiver_.is_bound())
      receiver_.reset();

    receiver_.Bind(std::move(pending_receiver_));
    receiver_.set_disconnect_handler(base::BindOnce(
        &ElectronApiServiceImpl::OnConnectionError, GetWeakPtr()));
  }
}

void ElectronApiServiceImpl::OnDestruct() {
  delete this;
}

void ElectronApiServiceImpl::OnConnectionError() {
  if (receiver_.is_bound())
    receiver_.reset();
}

void ElectronApiServiceImpl::Message(bool internal,
                                     const std::string& channel,
                                     blink::CloneableMessage arguments,
                                     int32_t sender_id) {
  // Don't handle browser messages before document element is created - instead,
  // save the messages and then replay them after the document is ready.
  //
  // See: https://chromium-review.googlesource.com/c/chromium/src/+/2601063.
  if (!document_created_) {
    PendingElectronApiServiceMessage message;
    message.internal = internal;
    message.channel = channel;
    message.arguments = std::move(arguments);
    message.sender_id = sender_id;

    pending_messages_.push(std::move(message));
  }

  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame)
    return;

  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Context> context = renderer_client_->GetContext(frame, isolate);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Value> args = gin::ConvertToV8(isolate, arguments);

  EmitIPCEvent(context, internal, channel, {}, args, sender_id);
}

void ElectronApiServiceImpl::ReceivePostMessage(
    const std::string& channel,
    blink::TransferableMessage message) {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame)
    return;

  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Context> context = renderer_client_->GetContext(frame, isolate);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Value> message_value = DeserializeV8Value(isolate, message);

  std::vector<v8::Local<v8::Value>> ports;
  for (auto& port : message.ports) {
    ports.emplace_back(
        blink::WebMessagePortConverter::EntangleAndInjectMessagePortChannel(
            context, std::move(port)));
  }

  std::vector<v8::Local<v8::Value>> args = {message_value};

  EmitIPCEvent(context, false, channel, ports, gin::ConvertToV8(isolate, args),
               0);
}

void ElectronApiServiceImpl::NotifyUserActivation() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (frame)
    frame->NotifyUserActivation(
        blink::mojom::UserActivationNotificationType::kInteraction);
}

void ElectronApiServiceImpl::TakeHeapSnapshot(
    mojo::ScopedHandle file,
    TakeHeapSnapshotCallback callback) {
  base::ThreadRestrictions::ScopedAllowIO allow_io;

  base::ScopedPlatformFile platform_file;
  if (mojo::UnwrapPlatformFile(std::move(file), &platform_file) !=
      MOJO_RESULT_OK) {
    LOG(ERROR) << "Unable to get the file handle from mojo.";
    std::move(callback).Run(false);
    return;
  }
  base::File base_file(std::move(platform_file));

  bool success =
      electron::TakeHeapSnapshot(blink::MainThreadIsolate(), &base_file);

  std::move(callback).Run(success);
}

}  // namespace electron
