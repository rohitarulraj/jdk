/*
 * Copyright (c) 2018, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_RUNTIME_VFRAME_INLINE_HPP
#define SHARE_RUNTIME_VFRAME_INLINE_HPP

#include "runtime/vframe.hpp"

#include "classfile/javaClasses.inline.hpp"
#include "oops/stackChunkOop.inline.hpp"
#include "runtime/continuationJavaClasses.inline.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/javaThread.inline.hpp"

inline vframeStreamCommon::vframeStreamCommon(JavaThread* thread,
                                              RegisterMap::UpdateMap update_map,
                                              RegisterMap::ProcessFrames process_frames,
                                              RegisterMap::WalkContinuation walk_cont)
        : _reg_map(thread, update_map, process_frames, walk_cont), _cont_entry(nullptr) {
  _thread = _reg_map.thread();
}

inline oop vframeStreamCommon::continuation() const {
  if (_reg_map.cont() != nullptr) {
    return _reg_map.cont();
  } else if (_cont_entry != nullptr) {
    return _cont_entry->cont_oop(_reg_map.thread());
  } else {
    return nullptr;
  }
}

inline intptr_t* vframeStreamCommon::frame_id() const {
  if (_frame.is_heap_frame()) {
    // Make something sufficiently unique
    intptr_t id = _reg_map.stack_chunk_index() << 16;
    id += _frame.offset_unextended_sp();
    return reinterpret_cast<intptr_t*>(id);
  }
  return _frame.id();
}

inline int vframeStreamCommon::vframe_id() const {
  assert(_mode == compiled_mode, "unexpected mode: %d", _mode);
  return _vframe_id;
}

inline int vframeStreamCommon::decode_offset() const {
  assert(_mode == compiled_mode, "unexpected mode: %d", _mode);
  return _decode_offset;
}

inline bool vframeStreamCommon::is_interpreted_frame() const { return _frame.is_interpreted_frame(); }

inline void vframeStreamCommon::next() {
  // handle frames with inlining
  if (_mode == compiled_mode    && fill_in_compiled_inlined_sender()) return;

  // handle general case
  do {
    bool is_enterSpecial_frame  = false;
    if (Continuation::is_continuation_enterSpecial(_frame)) {
      assert(!_reg_map.in_cont(), "");
      assert(_cont_entry != nullptr, "");
      // Reading oops are only safe if process_frames() is true, and we fix the oops.
      assert(!_reg_map.process_frames() || _cont_entry->cont_oop(_reg_map.thread()) != nullptr, "_cont: " INTPTR_FORMAT, p2i(_cont_entry));
      is_enterSpecial_frame = true;

      // TODO: handle ShowCarrierFrames
      if (_cont_entry->is_virtual_thread() ||
          (_continuation_scope.not_null() && _cont_entry->scope(_reg_map.thread()) == _continuation_scope())) {
        _mode = at_end_mode;
        break;
      }
    } else if (_reg_map.in_cont() && Continuation::is_continuation_entry_frame(_frame, &_reg_map)) {
      assert(_reg_map.cont() != nullptr, "");
      oop scope = jdk_internal_vm_Continuation::scope(_reg_map.cont());
      if (scope == java_lang_VirtualThread::vthread_scope() ||
          (_continuation_scope.not_null() && scope == _continuation_scope())) {
        _mode = at_end_mode;
        break;
      }
    }

    _frame = _frame.sender(&_reg_map);

    if (is_enterSpecial_frame) {
      _cont_entry = _cont_entry->parent();
    }
  } while (!fill_from_frame());
}

inline vframeStream::vframeStream(JavaThread* thread, bool stop_at_java_call_stub, bool process_frame, bool vthread_carrier)
  : vframeStreamCommon(thread,
                       RegisterMap::UpdateMap::include,
                       process_frame ? RegisterMap::ProcessFrames::include : RegisterMap::ProcessFrames::skip ,
                       RegisterMap::WalkContinuation::include) {
  _stop_at_java_call_stub = stop_at_java_call_stub;

  if (!thread->has_last_Java_frame()) {
    _mode = at_end_mode;
    return;
  }

  if (thread->is_vthread_mounted()) {
    _frame = vthread_carrier ? _thread->carrier_last_frame(&_reg_map) : _thread->vthread_last_frame();
    if (Continuation::is_continuation_enterSpecial(_frame)) {
      // This can happen when calling async_get_stack_trace() and catching the target
      // vthread at the JRT_BLOCK_END in freeze_internal() or when posting the Monitor
      // Waited event after target vthread was preempted. Since all continuation frames
      // are freezed we get the top frame from the stackChunk instead.
      _frame = Continuation::last_frame(java_lang_VirtualThread::continuation(_thread->vthread()), &_reg_map);
    }
  } else {
    _frame = _thread->last_frame();
  }

  _cont_entry = _thread->last_continuation();
  while (!fill_from_frame()) {
    _frame = _frame.sender(&_reg_map);
  }
}

inline bool vframeStreamCommon::fill_in_compiled_inlined_sender() {
  if (_sender_decode_offset == DebugInformationRecorder::serialized_null) {
    return false;
  }
  fill_from_compiled_frame(_sender_decode_offset);
  ++_vframe_id;
  return true;
}


inline void vframeStreamCommon::fill_from_compiled_frame(int decode_offset) {
  _mode = compiled_mode;
  _decode_offset = decode_offset;

  // Range check to detect ridiculous offsets.
  if (decode_offset == DebugInformationRecorder::serialized_null ||
      decode_offset < 0 ||
      decode_offset >= nm()->scopes_data_size()) {
    // 6379830 AsyncGetCallTrace sometimes feeds us wild frames.
    // If we read nmethod::scopes_data at serialized_null (== 0)
    // or if read some at other invalid offset, invalid values will be decoded.
    // Based on these values, invalid heap locations could be referenced
    // that could lead to crashes in product mode.
    // Therefore, do not use the decode offset if invalid, but fill the frame
    // as it were a native compiled frame (no Java-level assumptions).
#ifdef ASSERT
    if (WizardMode) {
      // Keep tty output consistent. To avoid ttyLocker, we buffer in stream, and print all at once.
      stringStream ss;
      ss.print_cr("Error in fill_from_frame: pc_desc for "
                  INTPTR_FORMAT " not found or invalid at %d",
                  p2i(_frame.pc()), decode_offset);
      nm()->print_on(&ss);
      nm()->method()->print_codes_on(&ss);
      nm()->print_code_on(&ss);
      nm()->print_pcs_on(&ss);
      tty->print("%s", ss.as_string()); // print all at once
    }
    found_bad_method_frame();
#endif
    // Provide a cheap fallback in product mode.  (See comment above.)
    fill_from_compiled_native_frame();
    return;
  }

  // Decode first part of scopeDesc
  DebugInfoReadStream buffer(nm(), decode_offset);
  _sender_decode_offset = buffer.read_int();
  _method               = buffer.read_method();
  _bci                  = buffer.read_bci();

  assert(_method->is_method(), "checking type of decoded method");
}

// The native frames are handled specially. We do not rely on ScopeDesc info
// since the pc might not be exact due to the _last_native_pc trick.
inline void vframeStreamCommon::fill_from_compiled_native_frame() {
  _mode = compiled_mode;
  _sender_decode_offset = DebugInformationRecorder::serialized_null;
  _decode_offset = DebugInformationRecorder::serialized_null;
  _vframe_id = 0;
  _method = nm()->method();
  _bci = 0;
}

inline bool vframeStreamCommon::fill_from_frame() {
  // Interpreted frame
  if (_frame.is_interpreted_frame()) {
    fill_from_interpreter_frame();
    return true;
  }

  // Compiled frame

  if (cb() != nullptr && cb()->is_nmethod()) {
    assert(nm()->method() != nullptr, "must be");
    if (nm()->is_native_method()) {
      // Do not rely on scopeDesc since the pc might be imprecise due to the _last_native_pc trick.
      fill_from_compiled_native_frame();
    } else {
      PcDesc* pc_desc = nm()->pc_desc_at(_frame.pc());
      int decode_offset;
      if (pc_desc == nullptr) {
        // Should not happen, but let fill_from_compiled_frame handle it.

        // If we are trying to walk the stack of a thread that is not
        // at a safepoint (like AsyncGetCallTrace would do) then this is an
        // acceptable result. [ This is assuming that safe_for_sender
        // is so bullet proof that we can trust the frames it produced. ]
        //
        // So if we see that the thread is not safepoint safe
        // then simply produce the method and a bci of zero
        // and skip the possibility of decoding any inlining that
        // may be present. That is far better than simply stopping (or
        // asserting. If however the thread is safepoint safe this
        // is the sign of a compiler bug  and we'll let
        // fill_from_compiled_frame handle it.


        JavaThreadState state = _thread != nullptr ? _thread->thread_state() : _thread_in_Java;

        // in_Java should be good enough to test safepoint safety
        // if state were say in_Java_trans then we'd expect that
        // the pc would have already been slightly adjusted to
        // one that would produce a pcDesc since the trans state
        // would be one that might in fact anticipate a safepoint

        if (state == _thread_in_Java ) {
          // This will get a method a zero bci and no inlining.
          // Might be nice to have a unique bci to signify this
          // particular case but for now zero will do.

          fill_from_compiled_native_frame();

          // There is something to be said for setting the mode to
          // at_end_mode to prevent trying to walk further up the
          // stack. There is evidence that if we walk any further
          // that we could produce a bad stack chain. However until
          // we see evidence that allowing this causes us to find
          // frames bad enough to cause segv's or assertion failures
          // we don't do it as while we may get a bad call chain the
          // probability is much higher (several magnitudes) that we
          // get good data.

          return true;
        }
        decode_offset = DebugInformationRecorder::serialized_null;
      } else {
        decode_offset = pc_desc->scope_decode_offset();
      }
      fill_from_compiled_frame(decode_offset);

      _vframe_id = 0;
    }
    return true;
  }

  // End of stack?
  if (_frame.is_first_frame() || (_stop_at_java_call_stub && _frame.is_entry_frame())) {
    _mode = at_end_mode;
    return true;
  }

  assert(!Continuation::is_continuation_enterSpecial(_frame), "");
  return false;
}


inline void vframeStreamCommon::fill_from_interpreter_frame() {
  Method* method;
  address bcp;
  if (!_reg_map.in_cont()) {
    method = _frame.interpreter_frame_method();
    bcp    = _frame.interpreter_frame_bcp();
  } else {
    method = _reg_map.stack_chunk()->interpreter_frame_method(_frame);
    bcp    = _reg_map.stack_chunk()->interpreter_frame_bcp(_frame);
  }
  int bci  = method->validate_bci_from_bcp(bcp);
  // 6379830 AsyncGetCallTrace sometimes feeds us wild frames.
  // AsyncGetCallTrace interrupts the VM asynchronously. As a result
  // it is possible to access an interpreter frame for which
  // no Java-level information is yet available (e.g., because
  // the frame was being created when the VM interrupted it).
  // In this scenario, pretend that the interpreter is at the point
  // of entering the method.
  if (bci < 0) {
    DEBUG_ONLY(found_bad_method_frame();)
    bci = 0;
  }
  _mode   = interpreted_mode;
  _method = method;
  _bci    = bci;
}

#endif // SHARE_RUNTIME_VFRAME_INLINE_HPP
