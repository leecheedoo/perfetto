/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_redaction/redact_ftrace_events.h"

#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

FtraceEventFilter::~FtraceEventFilter() = default;

FtraceEventWriter::~FtraceEventWriter() = default;

bool FilterFtracesUsingAllowlist::Includes(const Context& context,
                                           protozero::Field event) const {
  PERFETTO_DCHECK(!context.ftrace_packet_allow_list.empty());

  protozero::ProtoDecoder decoder(event.as_bytes());

  for (auto it = decoder.ReadField(); it.valid(); it = decoder.ReadField()) {
    if (context.ftrace_packet_allow_list.count(it.id())) {
      return true;
    }
  }

  return false;
}

bool FilterFtraceUsingSuspendResume::Includes(const Context&,
                                              protozero::Field event) const {
  // Values are taken from "suspend_period.textproto". These values would
  // ideally be provided via the context, but until there are multiple sources,
  // they can be here.
  constexpr std::string_view kSyscoreSuspend = "syscore_suspend";
  constexpr std::string_view kSyscoreResume = "syscore_resume";
  constexpr std::string_view kTimekeepingFreeze = "timekeeping_freeze";

  protozero::ProtoDecoder event_decoder(event.as_bytes());

  // It's not a suspend-resume event, defer the decision to another filter.
  auto suspend_resume = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kSuspendResumeFieldNumber);

  if (!suspend_resume.valid()) {
    return true;
  }

  protozero::ProtoDecoder suspend_resume_decoder(suspend_resume.as_bytes());

  auto action = suspend_resume_decoder.FindField(
      protos::pbzero::SuspendResumeFtraceEvent::kActionFieldNumber);

  // If a suspend-resume has no action, there is nothing to redact, so it is
  // safe to passthrough.
  if (!action.valid()) {
    return true;
  }

  std::string_view action_str(action.as_string().data, action.size());

  return kSyscoreSuspend == action_str || kSyscoreResume == action_str ||
         kTimekeepingFreeze == action_str;
}

base::Status WriteFtracesPassthrough::WriteTo(
    const Context&,
    protozero::Field event,
    protos::pbzero::FtraceEventBundle* message) const {
  proto_util::AppendField(event, message);
  return base::OkStatus();
}

base::Status RedactFtraceEvents::Transform(const Context& context,
                                           std::string* packet) const {
  if (packet == nullptr || packet->empty()) {
    return base::ErrStatus("RedactFtraceEvents: null or empty packet.");
  }

  if (!HasFtraceEvent(*packet)) {
    return base::OkStatus();
  }

  protozero::ProtoDecoder decoder(*packet);

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  for (auto it = decoder.ReadField(); it.valid(); it = decoder.ReadField()) {
    if (it.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      RETURN_IF_ERROR(
          OnFtraceEvents(context, it.as_bytes(), message->set_ftrace_events()));
    } else {
      proto_util::AppendField(it, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

bool RedactFtraceEvents::HasFtraceEvent(const std::string& bytes) const {
  protozero::ProtoDecoder packet(bytes);

  auto field =
      packet.FindField(protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  if (!field.valid()) {
    return false;
  }

  protozero::ProtoDecoder events(field.as_bytes());

  // Because kEventFieldNumber is a repeated field, FindField() doesn't work.
  for (auto it = events.ReadField(); it.valid(); it = events.ReadField()) {
    if (it.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      return true;
    }
  }

  return false;
}

base::Status RedactFtraceEvents::OnFtraceEvents(
    const Context& context,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEventBundle* message) const {
  protozero::ProtoDecoder bundle(bytes);

  for (auto it = bundle.ReadField(); it.valid(); it = bundle.ReadField()) {
    if (it.id() == protos::pbzero::FtraceEventBundle::kEventFieldNumber) {
      if (filter_->Includes(context, it)) {
        RETURN_IF_ERROR(writer_->WriteTo(context, it, message));
      }
    } else {
      proto_util::AppendField(it, message);
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
