/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "irregexp/NativeRegExpMacroAssembler.h"

#include "irregexp/RegExpStack.h"
#include "jit/IonLinker.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "vm/MatchPairs.h"

using namespace js;
using namespace js::irregexp;
using namespace js::jit;

/*
 * This assembler uses the following register assignment convention:
 *
 * - current_character :
 *     Must be loaded using LoadCurrentCharacter before using any of the
 *     dispatch methods. Temporarily stores the index of capture start after a
 *     matching pass for a global regexp.
 * - current_position :
 *     Current position in input, as negative offset from end of string.
 *     Please notice that this is the byte offset, not the character offset!
 * - input_end_pointer :
 *     Points to byte after last character in the input.
 * - backtrack_stack_pointer :
 *     Points to tip of the heap allocated backtrack stack
 * - StackPointer :
 *     Points to tip of the native stack, used to access arguments, local
 *     variables and RegExp registers.
 *
 * The tempN registers are free to use for computations.
 */

NativeRegExpMacroAssembler::NativeRegExpMacroAssembler(LifoAlloc *alloc, RegExpShared *shared,
                                                       JSRuntime *rt, Mode mode, int registers_to_save)
  : RegExpMacroAssembler(*alloc, shared, registers_to_save),
    runtime(rt), mode_(mode)
{
    // Find physical registers for each compiler register.
    GeneralRegisterSet regs(GeneralRegisterSet::All());

    input_end_pointer = regs.takeAny();
    current_character = regs.takeAny();
    current_position = regs.takeAny();
    backtrack_stack_pointer = regs.takeAny();
    temp0 = regs.takeAny();
    temp1 = regs.takeAny();
    temp2 = regs.takeAny();

    IonSpew(IonSpew_Codegen,
            "Starting RegExp (input_end_pointer %s) (current_character %s)"
            " (current_position %s) (backtrack_stack_pointer %s) (temp0 %s) temp1 (%s) temp2 (%s)",
            input_end_pointer.name(),
            current_character.name(),
            current_position.name(),
            backtrack_stack_pointer.name(),
            temp0.name(),
            temp1.name(),
            temp2.name());

    // Determine the non-volatile registers which might be modified by jitcode.
    for (GeneralRegisterIterator iter(GeneralRegisterSet::NonVolatile()); iter.more(); iter++) {
        Register reg = *iter;
        if (!regs.has(reg))
            savedNonVolatileRegisters.add(reg);
    }

#ifdef JS_CODEGEN_ARM
    // ARM additionally requires that the link register be saved.
    savedNonVolatileRegisters.add(Register::FromCode(Registers::lr));
#endif

    masm.jump(&entry_label_);
    masm.bind(&start_label_);
}

#define SPEW_PREFIX IonSpew_Codegen, "!!! "

// The signature of the code which this generates is:
//
// void execute(InputOutputData*);
RegExpCode
NativeRegExpMacroAssembler::GenerateCode(JSContext *cx)
{
    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return RegExpCode();

    IonSpew(SPEW_PREFIX "GenerateCode");

    // We need an even number of registers, for stack alignment.
    if (num_registers_ % 2 == 1)
        num_registers_++;

    Label return_temp0;

    // Finalize code - write the entry point code now we know how many
    // registers we need.
    masm.bind(&entry_label_);

    // Push non-volatile registers which might be modified by jitcode.
    size_t pushedNonVolatileRegisters = 0;
    for (GeneralRegisterForwardIterator iter(savedNonVolatileRegisters); iter.more(); ++iter) {
        masm.Push(*iter);
        pushedNonVolatileRegisters++;
    }

#ifndef JS_CODEGEN_X86
    // The InputOutputData* is stored as an argument, save it on the stack
    // above the frame.
    masm.Push(IntArgReg0);
#endif

    size_t frameSize = sizeof(FrameData) + num_registers_ * sizeof(void *);
    frameSize = JS_ROUNDUP(frameSize + masm.framePushed(), StackAlignment) - masm.framePushed();

    // Actually emit code to start a new stack frame.
    masm.reserveStack(frameSize);
    masm.checkStackAlignment();

    // Check if we have space on the stack.
    Label stack_ok;
    void *stack_limit = &runtime->mainThread.jitStackLimit;
    masm.branchPtr(Assembler::Below, AbsoluteAddress(stack_limit), StackPointer, &stack_ok);

    // Exit with an exception. There is not enough space on the stack
    // for our working registers.
    masm.mov(ImmWord(RegExpRunStatus_Error), temp0);
    masm.jump(&return_temp0);

    masm.bind(&stack_ok);

#ifdef XP_WIN
    // Ensure that we write to each stack page, in order. Skipping a page
    // on Windows can cause segmentation faults. Assuming page size is 4k.
    const int kPageSize = 4096;
    for (int i = frameSize - sizeof(void *); i >= 0; i -= kPageSize)
        masm.storePtr(temp0, Address(StackPointer, i));
#endif // XP_WIN

#ifndef JS_CODEGEN_X86
    // The InputOutputData* is stored on the stack immediately above the frame.
    Address inputOutputAddress(StackPointer, frameSize);
#else
    // The InputOutputData* is left in its original on stack location.
    Address inputOutputAddress(StackPointer, frameSize + (pushedNonVolatileRegisters + 1) * sizeof(void *));
#endif

    masm.loadPtr(inputOutputAddress, temp0);

    // Copy output registers to FrameData.
    {
        Register matchPairsRegister = input_end_pointer;
        masm.loadPtr(Address(temp0, offsetof(InputOutputData, matches)), matchPairsRegister);
        masm.loadPtr(Address(matchPairsRegister, MatchPairs::offsetOfPairs()), temp1);
        masm.storePtr(temp1, Address(StackPointer, offsetof(FrameData, outputRegisters)));
        masm.load32(Address(matchPairsRegister, MatchPairs::offsetOfPairCount()), temp1);
        masm.lshiftPtr(Imm32(1), temp1);
        masm.store32(temp1, Address(StackPointer, offsetof(FrameData, numOutputRegisters)));

#ifdef DEBUG
        // Bounds check numOutputRegisters.
        Label enoughRegisters;
        masm.cmpPtr(temp1, ImmWord(num_saved_registers_));
        masm.j(Assembler::GreaterThanOrEqual, &enoughRegisters);
        masm.assumeUnreachable("Not enough output registers for RegExp");
        masm.bind(&enoughRegisters);
#endif
    }

    // Load string end pointer.
    masm.loadPtr(Address(temp0, offsetof(InputOutputData, inputEnd)), input_end_pointer);

    // Load input start pointer, and copy to FrameData.
    masm.loadPtr(Address(temp0, offsetof(InputOutputData, inputStart)), current_position);
    masm.storePtr(current_position, Address(StackPointer, offsetof(FrameData, inputStart)));

    // Load start index, and copy to FrameData.
    masm.loadPtr(Address(temp0, offsetof(InputOutputData, startIndex)), temp1);
    masm.storePtr(temp1, Address(StackPointer, offsetof(FrameData, startIndex)));

    // Set up input position to be negative offset from string end.
    masm.subPtr(input_end_pointer, current_position);

    // Set temp0 to address of char before start of the string.
    // (effectively string position -1).
    masm.computeEffectiveAddress(Address(current_position, -char_size()), temp0);

    // Store this value on the frame, for use when clearing
    // position registers.
    masm.storePtr(temp0, Address(StackPointer, offsetof(FrameData, inputStartMinusOne)));

    // Update current position based on start index.
    masm.computeEffectiveAddress(BaseIndex(current_position, temp1, factor()), current_position);

    Label load_char_start_regexp, start_regexp;

    // Load newline if index is at start, previous character otherwise.
    masm.cmpPtr(Address(StackPointer, offsetof(FrameData, startIndex)), ImmWord(0));
    masm.j(Assembler::NotEqual, &load_char_start_regexp);
    masm.mov(ImmWord('\n'), current_character);
    masm.jump(&start_regexp);

    // Global regexp restarts matching here.
    masm.bind(&load_char_start_regexp);

    // Load previous char as initial value of current character register.
    LoadCurrentCharacterUnchecked(-1, 1);
    masm.bind(&start_regexp);

    // Initialize on-stack registers.
    JS_ASSERT(num_saved_registers_ > 0);

    // Fill saved registers with initial value = start offset - 1
    // Fill in stack push order, to avoid accessing across an unwritten
    // page (a problem on Windows).
    if (num_saved_registers_ > 8) {
        masm.mov(ImmWord(register_offset(0)), temp1);
        Label init_loop;
        masm.bind(&init_loop);
        masm.storePtr(temp0, BaseIndex(StackPointer, temp1, TimesOne));
        masm.addPtr(ImmWord(sizeof(void *)), temp1);
        masm.cmpPtr(temp1, ImmWord(register_offset(num_saved_registers_)));
        masm.j(Assembler::LessThan, &init_loop);
    } else {
        // Unroll the loop.
        for (int i = 0; i < num_saved_registers_; i++)
            masm.storePtr(temp0, register_location(i));
    }

    // Initialize backtrack stack pointer.
    masm.loadPtr(AbsoluteAddress(runtime->mainThread.regexpStack.addressOfBase()), backtrack_stack_pointer);
    masm.storePtr(backtrack_stack_pointer, Address(StackPointer, offsetof(FrameData, backtrackStackBase)));

    masm.jump(&start_label_);

    // Exit code:
    if (success_label_.used()) {
        JS_ASSERT(num_saved_registers_ > 0);

        Address outputRegistersAddress(StackPointer, offsetof(FrameData, outputRegisters));

        // Save captures when successful.
        masm.bind(&success_label_);

        {
            Register outputRegisters = temp1;
            Register inputByteLength = backtrack_stack_pointer;

            masm.loadPtr(outputRegistersAddress, outputRegisters);

            masm.loadPtr(inputOutputAddress, temp0);
            masm.loadPtr(Address(temp0, offsetof(InputOutputData, inputEnd)), inputByteLength);
            masm.subPtr(Address(temp0, offsetof(InputOutputData, inputStart)), inputByteLength);

            // Copy captures to output. Note that registers on the C stack are pointer width
            // so that they might hold pointers, but output registers are int32_t.
            for (int i = 0; i < num_saved_registers_; i++) {
                masm.loadPtr(register_location(i), temp0);
                if (i == 0 && global_with_zero_length_check()) {
                    // Keep capture start in current_character for the zero-length check later.
                    masm.mov(temp0, current_character);
                }

                // Convert to index from start of string, not end.
                masm.addPtr(inputByteLength, temp0);

                // Convert byte index to character index.
                if (mode_ == JSCHAR)
                    masm.rshiftPtrArithmetic(Imm32(1), temp0);

                masm.store32(temp0, Address(outputRegisters, i * sizeof(int32_t)));
            }
        }

        // Restart matching if the regular expression is flagged as global.
        if (global()) {
            // Increment success counter.
            masm.add32(Imm32(1), Address(StackPointer, offsetof(FrameData, successfulCaptures)));

            Address numOutputRegistersAddress(StackPointer, offsetof(FrameData, numOutputRegisters));

            // Capture results have been stored, so the number of remaining global
            // output registers is reduced by the number of stored captures.
            masm.load32(numOutputRegistersAddress, temp0);

            masm.sub32(Imm32(num_saved_registers_), temp0);

            // Check whether we have enough room for another set of capture results.
            masm.branch32(Assembler::LessThan, temp0, Imm32(num_saved_registers_), &exit_label_);

            masm.store32(temp0, numOutputRegistersAddress);

            // Advance the location for output.
            masm.add32(Imm32(num_saved_registers_ * sizeof(void *)), outputRegistersAddress);

            // Prepare temp0 to initialize registers with its value in the next run.
            masm.loadPtr(Address(StackPointer, offsetof(FrameData, inputStartMinusOne)), temp0);

            if (global_with_zero_length_check()) {
                // Special case for zero-length matches.

                // The capture start index was loaded into current_character above.
                masm.branchPtr(Assembler::NotEqual, current_position, current_character,
                               &load_char_start_regexp);

                // edi (offset from the end) is zero if we already reached the end.
                masm.testPtr(current_position, current_position);
                masm.j(Assembler::Zero, &exit_label_);

                // Advance current position after a zero-length match.
                masm.addPtr(Imm32(char_size()), current_position);
            }

            masm.jump(&load_char_start_regexp);
        } else {
            masm.mov(ImmWord(RegExpRunStatus_Success), temp0);
        }
    }

    masm.bind(&exit_label_);

    if (global()) {
        // Return the number of successful captures.
        masm.load32(Address(StackPointer, offsetof(FrameData, successfulCaptures)), temp0);
    }

    masm.bind(&return_temp0);

    // Store the result to the input structure.
    masm.loadPtr(inputOutputAddress, temp1);
    masm.storePtr(temp0, Address(temp1, offsetof(InputOutputData, result)));

#ifndef JS_CODEGEN_X86
    // Include the InputOutputData* when adjusting the stack size.
    masm.freeStack(frameSize + sizeof(void *));
#else
    masm.freeStack(frameSize);
#endif

    // Restore non-volatile registers which were saved on entry.
    for (GeneralRegisterBackwardIterator iter(savedNonVolatileRegisters); iter.more(); ++iter)
        masm.Pop(*iter);

    masm.abiret();

    // Backtrack code (branch target for conditional backtracks).
    if (backtrack_label_.used()) {
        masm.bind(&backtrack_label_);
        Backtrack();
    }

    // Backtrack stack overflow code.
    if (stack_overflow_label_.used()) {
        // Reached if the backtrack-stack limit has been hit. temp2 holds the
        // StackPointer to use for accessing FrameData.
        masm.bind(&stack_overflow_label_);

        Label grow_failed;

        masm.mov(ImmPtr(runtime), temp1);

        // Save registers before calling C function
        RegisterSet volatileRegs = RegisterSet::Volatile();
#ifdef JS_CODEGEN_ARM
        volatileRegs.add(Register::FromCode(Registers::lr));
#endif
        volatileRegs.takeUnchecked(temp0);
        volatileRegs.takeUnchecked(temp1);
        masm.PushRegsInMask(volatileRegs);

        masm.setupUnalignedABICall(1, temp0);
        masm.passABIArg(temp1);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, GrowBacktrackStack));
        masm.storeCallResult(temp0);

        masm.PopRegsInMask(volatileRegs);

        // If return false, we have failed to grow the stack, and
        // must exit with a stack-overflow exception. Do this in the caller
        // so that the stack is adjusted by our return instruction.
        Label return_from_overflow_handler;
        masm.branchTest32(Assembler::Zero, temp0, temp0, &return_from_overflow_handler);

        // Otherwise, store the new backtrack stack base and recompute the new
        // top of the stack.
        Address backtrackStackBaseAddress(temp2, offsetof(FrameData, backtrackStackBase));
        masm.subPtr(backtrackStackBaseAddress, backtrack_stack_pointer);

        masm.loadPtr(AbsoluteAddress(runtime->mainThread.regexpStack.addressOfBase()), temp1);
        masm.storePtr(temp1, backtrackStackBaseAddress);
        masm.addPtr(temp1, backtrack_stack_pointer);

        // Resume execution in calling code.
        masm.bind(&return_from_overflow_handler);
        masm.abiret();
    }

    if (exit_with_exception_label_.used()) {
        // If any of the code above needed to exit with an exception.
        masm.bind(&exit_with_exception_label_);

        // Exit with an error result to signal thrown exception.
        masm.mov(ImmWord(RegExpRunStatus_Error), temp0);
        masm.jump(&return_temp0);
    }

    Linker linker(masm);
    AutoFlushICache afc("RegExp");
    JitCode *code = linker.newCode<NoGC>(cx, JSC::REGEXP_CODE);
    if (!code)
        return RegExpCode();

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExp");
#endif

    for (size_t i = 0; i < labelPatches.length(); i++) {
        LabelPatch &v = labelPatches[i];
        JS_ASSERT(!v.label);
        v.patchOffset.fixup(&masm);
        uintptr_t offset = masm.actualOffset(v.labelOffset);
        Assembler::patchDataWithValueCheck(CodeLocationLabel(code, v.patchOffset),
                                           ImmPtr(code->raw() + offset),
                                           ImmPtr(0));
    }

    IonSpew(IonSpew_Codegen, "Created RegExp (raw %p length %d)",
            (void *) code->raw(), (int) masm.bytesNeeded());

    RegExpCode res;
    res.jitCode = code;
    return res;
}

int
NativeRegExpMacroAssembler::stack_limit_slack()
{
    return RegExpStack::kStackLimitSlack;
}

void
NativeRegExpMacroAssembler::AdvanceCurrentPosition(int by)
{
    IonSpew(SPEW_PREFIX "AdvanceCurrentPosition(%d)", by);

    if (by != 0)
        masm.addPtr(Imm32(by * char_size()), current_position);
}

void
NativeRegExpMacroAssembler::AdvanceRegister(int reg, int by)
{
    IonSpew(SPEW_PREFIX "AdvanceRegister(%d, %d)", reg, by);

    JS_ASSERT(reg >= 0);
    JS_ASSERT(reg < num_registers_);
    if (by != 0)
        masm.addPtr(Imm32(by), register_location(reg));
}

void
NativeRegExpMacroAssembler::Backtrack()
{
    IonSpew(SPEW_PREFIX "Backtrack");

    // Pop code location from backtrack stack and jump to location.
    PopBacktrack(temp0);
    masm.jump(temp0);
}

void
NativeRegExpMacroAssembler::Bind(Label *label)
{
    IonSpew(SPEW_PREFIX "Bind");

    masm.bind(label);
}

void
NativeRegExpMacroAssembler::CheckAtStart(Label* on_at_start)
{
    IonSpew(SPEW_PREFIX "CheckAtStart");

    Label not_at_start;

    // Did we start the match at the start of the string at all?
    masm.cmpPtr(Address(StackPointer, offsetof(FrameData, startIndex)), ImmWord(0));
    BranchOrBacktrack(Assembler::NotEqual, &not_at_start);

    // If we did, are we still at the start of the input?
    masm.computeEffectiveAddress(BaseIndex(input_end_pointer, current_position, TimesOne), temp0);
    masm.cmpPtr(Address(StackPointer, offsetof(FrameData, inputStart)), temp0);

    BranchOrBacktrack(Assembler::Equal, on_at_start);
    masm.bind(&not_at_start);
}

void
NativeRegExpMacroAssembler::CheckNotAtStart(Label* on_not_at_start)
{
    IonSpew(SPEW_PREFIX "CheckNotAtStart");

    // Did we start the match at the start of the string at all?
    masm.cmpPtr(Address(StackPointer, offsetof(FrameData, startIndex)), ImmWord(0));
    BranchOrBacktrack(Assembler::NotEqual, on_not_at_start);

    // If we did, are we still at the start of the input?
    masm.computeEffectiveAddress(BaseIndex(input_end_pointer, current_position, TimesOne), temp0);
    masm.cmpPtr(Address(StackPointer, offsetof(FrameData, inputStart)), temp0);
    BranchOrBacktrack(Assembler::NotEqual, on_not_at_start);
}

void
NativeRegExpMacroAssembler::CheckCharacter(unsigned c, Label* on_equal)
{
    IonSpew(SPEW_PREFIX "CheckCharacter(%d)", (int) c);

    masm.cmp32(current_character, Imm32(c));
    BranchOrBacktrack(Assembler::Equal, on_equal);
}

void
NativeRegExpMacroAssembler::CheckNotCharacter(unsigned c, Label* on_not_equal)
{
    IonSpew(SPEW_PREFIX "CheckNotCharacter(%d)", (int) c);

    masm.cmp32(current_character, Imm32(c));
    BranchOrBacktrack(Assembler::NotEqual, on_not_equal);
}

void
NativeRegExpMacroAssembler::CheckCharacterAfterAnd(unsigned c, unsigned and_with,
                                                   Label *on_equal)
{
    IonSpew(SPEW_PREFIX "CheckCharacterAfterAnd(%d, %d)", (int) c, (int) and_with);

    if (c == 0) {
        masm.test32(current_character, Imm32(and_with));
        BranchOrBacktrack(Assembler::Zero, on_equal);
    } else {
        masm.mov(ImmWord(and_with), temp0);
        masm.and32(current_character, temp0);
        masm.cmp32(temp0, Imm32(c));
        BranchOrBacktrack(Assembler::Equal, on_equal);
    }
}

void
NativeRegExpMacroAssembler::CheckNotCharacterAfterAnd(unsigned c, unsigned and_with,
                                                      Label *on_not_equal)
{
    IonSpew(SPEW_PREFIX "CheckNotCharacterAfterAnd(%d, %d)", (int) c, (int) and_with);

    if (c == 0) {
        masm.test32(current_character, Imm32(and_with));
        BranchOrBacktrack(Assembler::NonZero, on_not_equal);
    } else {
        masm.mov(ImmWord(and_with), temp0);
        masm.and32(current_character, temp0);
        masm.cmp32(temp0, Imm32(c));
        BranchOrBacktrack(Assembler::NotEqual, on_not_equal);
    }
}

void
NativeRegExpMacroAssembler::CheckCharacterGT(jschar c, Label* on_greater)
{
    IonSpew(SPEW_PREFIX "CheckCharacterGT(%d)", (int) c);

    masm.cmp32(current_character, Imm32(c));
    BranchOrBacktrack(Assembler::GreaterThan, on_greater);
}

void
NativeRegExpMacroAssembler::CheckCharacterLT(jschar c, Label* on_less)
{
    IonSpew(SPEW_PREFIX "CheckCharacterLT(%d)", (int) c);

    masm.cmp32(current_character, Imm32(c));
    BranchOrBacktrack(Assembler::LessThan, on_less);
}

void
NativeRegExpMacroAssembler::CheckGreedyLoop(Label* on_tos_equals_current_position)
{
    IonSpew(SPEW_PREFIX "CheckGreedyLoop");

    Label fallthrough;
    masm.cmpPtr(Address(backtrack_stack_pointer, -int(sizeof(void *))), current_position);
    masm.j(Assembler::NotEqual, &fallthrough);
    masm.subPtr(Imm32(sizeof(void *)), backtrack_stack_pointer);  // Pop.
    JumpOrBacktrack(on_tos_equals_current_position);
    masm.bind(&fallthrough);
}

void
NativeRegExpMacroAssembler::CheckNotBackReference(int start_reg, Label* on_no_match)
{
    IonSpew(SPEW_PREFIX "CheckNotBackReference(%d)", start_reg);

    Label fallthrough;
    Label success;
    Label fail;

    // Find length of back-referenced capture.
    masm.loadPtr(register_location(start_reg), current_character);
    masm.loadPtr(register_location(start_reg + 1), temp0);
    masm.subPtr(current_character, temp0);  // Length to check.
    masm.cmpPtr(temp0, ImmWord(0));

    // Fail on partial or illegal capture (start of capture after end of capture).
    BranchOrBacktrack(Assembler::LessThan, on_no_match);

    // Succeed on empty capture (including no capture).
    masm.j(Assembler::Equal, &fallthrough);

    // Check that there are sufficient characters left in the input.
    masm.mov(current_position, temp1);
    masm.addPtr(temp0, temp1);
    masm.cmpPtr(temp1, ImmWord(0));
    BranchOrBacktrack(Assembler::GreaterThan, on_no_match);

    // Save register to make it available below.
    masm.push(backtrack_stack_pointer);

    // Compute pointers to match string and capture string
    masm.computeEffectiveAddress(BaseIndex(input_end_pointer, current_position, TimesOne), temp1); // Start of match.
    masm.addPtr(input_end_pointer, current_character); // Start of capture.
    masm.computeEffectiveAddress(BaseIndex(temp0, temp1, TimesOne), backtrack_stack_pointer); // End of match.

    Label loop;
    masm.bind(&loop);
    if (mode_ == ASCII) {
        MOZ_ASSUME_UNREACHABLE("Ascii loading not implemented");
    } else {
        JS_ASSERT(mode_ == JSCHAR);
        masm.load16ZeroExtend(Address(current_character, 0), temp0);
        masm.load16ZeroExtend(Address(temp1, 0), temp2);
    }
    masm.branch32(Assembler::NotEqual, temp0, temp2, &fail);

    // Increment pointers into capture and match string.
    masm.addPtr(Imm32(char_size()), current_character);
    masm.addPtr(Imm32(char_size()), temp1);

    // Check if we have reached end of match area.
    masm.branchPtr(Assembler::Below, temp1, backtrack_stack_pointer, &loop);
    masm.jump(&success);

    masm.bind(&fail);

    // Restore backtrack stack pointer.
    masm.pop(backtrack_stack_pointer);
    JumpOrBacktrack(on_no_match);

    masm.bind(&success);

    // Move current character position to position after match.
    masm.mov(backtrack_stack_pointer, current_position);
    masm.subPtr(input_end_pointer, current_position);

    // Restore backtrack stack pointer.
    masm.pop(backtrack_stack_pointer);

    masm.bind(&fallthrough);
}

void
NativeRegExpMacroAssembler::CheckNotBackReferenceIgnoreCase(int start_reg, Label* on_no_match)
{
    IonSpew(SPEW_PREFIX "CheckNotBackReferenceIgnoreCase(%d)", start_reg);

    Label fallthrough;

    masm.loadPtr(register_location(start_reg), current_character);  // Index of start of capture
    masm.loadPtr(register_location(start_reg + 1), temp1);  // Index of end of capture
    masm.subPtr(current_character, temp1);  // Length of capture.
    masm.cmpPtr(temp1, ImmWord(0));

    // The length of a capture should not be negative. This can only happen
    // if the end of the capture is unrecorded, or at a point earlier than
    // the start of the capture.
    BranchOrBacktrack(Assembler::LessThan, on_no_match);

    // If length is zero, either the capture is empty or it is completely
    // uncaptured. In either case succeed immediately.
    masm.j(Assembler::Equal, &fallthrough);

    // Check that there are sufficient characters left in the input.
    masm.mov(current_position, temp0);
    masm.addPtr(temp1, temp0);
    masm.cmpPtr(temp0, ImmWord(0));
    BranchOrBacktrack(Assembler::GreaterThan, on_no_match);

    if (mode_ == ASCII) {
        MOZ_ASSUME_UNREACHABLE("Ascii case not implemented");
    } else {
        JS_ASSERT(mode_ == JSCHAR);

        // Note: temp1 needs to be saved/restored if it is volatile, as it is used after the call.
        RegisterSet volatileRegs = RegisterSet::Volatile();
        volatileRegs.takeUnchecked(temp0);
        volatileRegs.takeUnchecked(temp2);
        masm.PushRegsInMask(volatileRegs);

        // Set byte_offset1.
        // Start of capture, where current_character already holds string-end negative offset.
        masm.addPtr(input_end_pointer, current_character);

        // Set byte_offset2.
        // Found by adding negative string-end offset of current position
        // to end of string.
        masm.addPtr(input_end_pointer, current_position);

        // Parameters are
        //   Address byte_offset1 - Address captured substring's start.
        //   Address byte_offset2 - Address of current character position.
        //   size_t byte_length - length of capture in bytes(!)
        masm.setupUnalignedABICall(3, temp0);
        masm.passABIArg(current_character);
        masm.passABIArg(current_position);
        masm.passABIArg(temp1);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, CaseInsensitiveCompareStrings));
        masm.storeCallResult(temp0);

        masm.PopRegsInMask(volatileRegs);

        // Check if function returned non-zero for success or zero for failure.
        masm.test32(temp0, temp0);
        BranchOrBacktrack(Assembler::Zero, on_no_match);

        // On success, increment position by length of capture.
        masm.addPtr(temp1, current_position);
    }

    masm.bind(&fallthrough);
}

void
NativeRegExpMacroAssembler::CheckNotCharacterAfterMinusAnd(jschar c, jschar minus, jschar and_with,
                                                           Label* on_not_equal)
{
    IonSpew(SPEW_PREFIX "CheckNotCharacterAfterMinusAnd(%d, %d, %d)", (int) c, (int) minus, (int) and_with);

    masm.computeEffectiveAddress(Address(current_character, -minus), temp0);
    if (c == 0) {
        masm.test32(temp0, Imm32(and_with));
        BranchOrBacktrack(Assembler::NonZero, on_not_equal);
    } else {
        masm.and32(Imm32(and_with), temp0);
        masm.cmp32(temp0, Imm32(c));
        BranchOrBacktrack(Assembler::NotEqual, on_not_equal);
    }
}

void
NativeRegExpMacroAssembler::CheckCharacterInRange(jschar from, jschar to,
                                                  Label* on_in_range)
{
    IonSpew(SPEW_PREFIX "CheckCharacterInRange(%d, %d)", (int) from, (int) to);

    masm.computeEffectiveAddress(Address(current_character, -from), temp0);
    masm.cmp32(temp0, Imm32(to - from));
    BranchOrBacktrack(Assembler::BelowOrEqual, on_in_range);
}

void
NativeRegExpMacroAssembler::CheckCharacterNotInRange(jschar from, jschar to,
                                                     Label* on_not_in_range)
{
    IonSpew(SPEW_PREFIX "CheckCharacterNotInRange(%d, %d)", (int) from, (int) to);

    masm.computeEffectiveAddress(Address(current_character, -from), temp0);
    masm.cmp32(temp0, Imm32(to - from));
    BranchOrBacktrack(Assembler::Above, on_not_in_range);
}

void
NativeRegExpMacroAssembler::CheckBitInTable(uint8_t *table, Label *on_bit_set)
{
    IonSpew(SPEW_PREFIX "CheckBitInTable");

    JS_ASSERT(mode_ != ASCII); // Ascii case not handled here.

    masm.mov(ImmPtr(table), temp0);
    masm.mov(ImmWord(kTableSize - 1), temp1);
    masm.and32(current_character, temp1);

    masm.load8ZeroExtend(BaseIndex(temp0, temp1, TimesOne), temp0);
    masm.test32(temp0, temp0);
    BranchOrBacktrack(Assembler::NotEqual, on_bit_set);
}

void
NativeRegExpMacroAssembler::Fail()
{
    IonSpew(SPEW_PREFIX "Fail");

    if (!global())
        masm.mov(ImmWord(RegExpRunStatus_Success_NotFound), temp0);
    masm.jump(&exit_label_);
}

void
NativeRegExpMacroAssembler::IfRegisterGE(int reg, int comparand, Label* if_ge)
{
    IonSpew(SPEW_PREFIX "IfRegisterGE(%d, %d)", reg, comparand);

    masm.cmpPtr(register_location(reg), ImmWord(comparand));
    BranchOrBacktrack(Assembler::GreaterThanOrEqual, if_ge);
}

void
NativeRegExpMacroAssembler::IfRegisterLT(int reg, int comparand, Label* if_lt)
{
    IonSpew(SPEW_PREFIX "IfRegisterLT(%d, %d)", reg, comparand);

    masm.cmpPtr(register_location(reg), ImmWord(comparand));
    BranchOrBacktrack(Assembler::LessThan, if_lt);
}

void
NativeRegExpMacroAssembler::IfRegisterEqPos(int reg, Label* if_eq)
{
    IonSpew(SPEW_PREFIX "IfRegisterEqPos(%d)", reg);

    masm.cmpPtr(register_location(reg), current_position);
    BranchOrBacktrack(Assembler::Equal, if_eq);
}

void
NativeRegExpMacroAssembler::LoadCurrentCharacter(int cp_offset, Label* on_end_of_input,
                                                 bool check_bounds, int characters)
{
    IonSpew(SPEW_PREFIX "LoadCurrentCharacter(%d, %d)", cp_offset, characters);

    JS_ASSERT(cp_offset >= -1);      // ^ and \b can look behind one character.
    JS_ASSERT(cp_offset < (1<<30));  // Be sane! (And ensure negation works)
    if (check_bounds)
        CheckPosition(cp_offset + characters - 1, on_end_of_input);
    LoadCurrentCharacterUnchecked(cp_offset, characters);
}

void
NativeRegExpMacroAssembler::LoadCurrentCharacterUnchecked(int cp_offset, int characters)
{
    IonSpew(SPEW_PREFIX "LoadCurrentCharacterUnchecked(%d, %d)", cp_offset, characters);

    if (mode_ == ASCII) {
        MOZ_ASSUME_UNREACHABLE("Ascii loading not implemented");
    } else {
        JS_ASSERT(mode_ == JSCHAR);
        JS_ASSERT(characters <= 2);
        BaseIndex address(input_end_pointer, current_position, TimesOne, cp_offset * sizeof(jschar));
        if (characters == 2)
            masm.load32(address, current_character);
        else
            masm.load16ZeroExtend(address, current_character);
    }
}

void
NativeRegExpMacroAssembler::PopCurrentPosition()
{
    IonSpew(SPEW_PREFIX "PopCurrentPosition");

    PopBacktrack(current_position);
}

void
NativeRegExpMacroAssembler::PopRegister(int register_index)
{
    IonSpew(SPEW_PREFIX "PopRegister(%d)", register_index);

    PopBacktrack(temp0);
    masm.storePtr(temp0, register_location(register_index));
}

void
NativeRegExpMacroAssembler::PushBacktrack(Label *label)
{
    IonSpew(SPEW_PREFIX "PushBacktrack");

    CodeOffsetLabel patchOffset = masm.movWithPatch(ImmPtr(nullptr), temp0);

    JS_ASSERT(!label->bound());
    if (!labelPatches.append(LabelPatch(label, patchOffset)))
        CrashAtUnhandlableOOM("NativeRegExpMacroAssembler::PushBacktrack");

    PushBacktrack(temp0);
    CheckBacktrackStackLimit();
}

void
NativeRegExpMacroAssembler::BindBacktrack(Label *label)
{
    IonSpew(SPEW_PREFIX "BindBacktrack");

    Bind(label);

    for (size_t i = 0; i < labelPatches.length(); i++) {
        LabelPatch &v = labelPatches[i];
        if (v.label == label) {
            v.labelOffset = label->offset();
            v.label = nullptr;
            break;
        }
    }
}

void
NativeRegExpMacroAssembler::PushBacktrack(Register source)
{
    IonSpew(SPEW_PREFIX "PushBacktrack");

    JS_ASSERT(source != backtrack_stack_pointer);

    // Notice: This updates flags, unlike normal Push.
    masm.storePtr(source, Address(backtrack_stack_pointer, 0));
    masm.addPtr(Imm32(sizeof(void *)), backtrack_stack_pointer);
}

void
NativeRegExpMacroAssembler::PushBacktrack(int32_t value)
{
    IonSpew(SPEW_PREFIX "PushBacktrack(%d)", (int) value);

    // Notice: This updates flags, unlike normal Push.
    masm.storePtr(ImmWord(value), Address(backtrack_stack_pointer, 0));
    masm.addPtr(Imm32(sizeof(void *)), backtrack_stack_pointer);
}

void
NativeRegExpMacroAssembler::PopBacktrack(Register target)
{
    IonSpew(SPEW_PREFIX "PopBacktrack");

    JS_ASSERT(target != backtrack_stack_pointer);

    // Notice: This updates flags, unlike normal Pop.
    masm.subPtr(Imm32(sizeof(void *)), backtrack_stack_pointer);
    masm.loadPtr(Address(backtrack_stack_pointer, 0), target);
}

void
NativeRegExpMacroAssembler::CheckBacktrackStackLimit()
{
    IonSpew(SPEW_PREFIX "CheckBacktrackStackLimit");

    const void *limitAddr = runtime->mainThread.regexpStack.addressOfLimit();

    Label no_stack_overflow;
    masm.branchPtr(Assembler::AboveOrEqual, AbsoluteAddress(limitAddr),
                   backtrack_stack_pointer, &no_stack_overflow);

    // Copy the stack pointer before the call() instruction modifies it.
    masm.mov(StackPointer, temp2);

    masm.call(&stack_overflow_label_);
    masm.bind(&no_stack_overflow);

    // Exit with an exception if the call failed.
    masm.test32(temp0, temp0);
    masm.j(Assembler::Zero, &exit_with_exception_label_);
}

void
NativeRegExpMacroAssembler::PushCurrentPosition()
{
    IonSpew(SPEW_PREFIX "PushCurrentPosition");

    PushBacktrack(current_position);
}

void
NativeRegExpMacroAssembler::PushRegister(int register_index, StackCheckFlag check_stack_limit)
{
    IonSpew(SPEW_PREFIX "PushRegister(%d)", register_index);

    masm.loadPtr(register_location(register_index), temp0);
    PushBacktrack(temp0);
    if (check_stack_limit)
        CheckBacktrackStackLimit();
}

void
NativeRegExpMacroAssembler::ReadCurrentPositionFromRegister(int reg)
{
    IonSpew(SPEW_PREFIX "ReadCurrentPositionFromRegister(%d)", reg);

    masm.loadPtr(register_location(reg), current_position);
}

void
NativeRegExpMacroAssembler::WriteCurrentPositionToRegister(int reg, int cp_offset)
{
    IonSpew(SPEW_PREFIX "WriteCurrentPositionToRegister(%d, %d)", reg, cp_offset);

    if (cp_offset == 0) {
        masm.storePtr(current_position, register_location(reg));
    } else {
        masm.computeEffectiveAddress(Address(current_position, cp_offset * char_size()), temp0);
        masm.storePtr(temp0, register_location(reg));
    }
}

void
NativeRegExpMacroAssembler::ReadBacktrackStackPointerFromRegister(int reg)
{
    IonSpew(SPEW_PREFIX "ReadBacktrackStackPointerFromRegister(%d)", reg);

    masm.loadPtr(register_location(reg), backtrack_stack_pointer);
    masm.addPtr(Address(StackPointer, offsetof(FrameData, backtrackStackBase)), backtrack_stack_pointer);
}

void
NativeRegExpMacroAssembler::WriteBacktrackStackPointerToRegister(int reg)
{
    IonSpew(SPEW_PREFIX "WriteBacktrackStackPointerToRegister(%d)", reg);

    masm.mov(backtrack_stack_pointer, temp0);
    masm.subPtr(Address(StackPointer, offsetof(FrameData, backtrackStackBase)), temp0);
    masm.storePtr(temp0, register_location(reg));
}

void
NativeRegExpMacroAssembler::SetCurrentPositionFromEnd(int by)
{
    IonSpew(SPEW_PREFIX "SetCurrentPositionFromEnd(%d)", by);

    Label after_position;
    masm.cmpPtr(current_position, ImmWord(-by * char_size()));
    masm.j(Assembler::GreaterThanOrEqual, &after_position);
    masm.mov(ImmWord(-by * char_size()), current_position);

    // On RegExp code entry (where this operation is used), the character before
    // the current position is expected to be already loaded.
    // We have advanced the position, so it's safe to read backwards.
    LoadCurrentCharacterUnchecked(-1, 1);
    masm.bind(&after_position);
}

void
NativeRegExpMacroAssembler::SetRegister(int register_index, int to)
{
    IonSpew(SPEW_PREFIX "SetRegister(%d, %d)", register_index, to);

    JS_ASSERT(register_index >= num_saved_registers_);  // Reserved for positions!
    masm.storePtr(ImmWord(to), register_location(register_index));
}

bool
NativeRegExpMacroAssembler::Succeed()
{
    IonSpew(SPEW_PREFIX "Succeed");

    masm.jump(&success_label_);
    return global();
}

void
NativeRegExpMacroAssembler::ClearRegisters(int reg_from, int reg_to)
{
    IonSpew(SPEW_PREFIX "ClearRegisters(%d, %d)", reg_from, reg_to);

    JS_ASSERT(reg_from <= reg_to);
    masm.loadPtr(Address(StackPointer, offsetof(FrameData, inputStartMinusOne)), temp0);
    for (int reg = reg_from; reg <= reg_to; reg++)
        masm.storePtr(temp0, register_location(reg));
}

void
NativeRegExpMacroAssembler::CheckPosition(int cp_offset, Label* on_outside_input)
{
    IonSpew(SPEW_PREFIX "CheckPosition(%d)", cp_offset);

    masm.cmpPtr(current_position, ImmWord(-cp_offset * char_size()));
    BranchOrBacktrack(Assembler::GreaterThanOrEqual, on_outside_input);
}

void
NativeRegExpMacroAssembler::BranchOrBacktrack(Assembler::Condition condition, Label *to)
{
    IonSpew(SPEW_PREFIX "BranchOrBacktrack");

    if (to)
        masm.j(condition, to);
    else
        masm.j(condition, &backtrack_label_);
}

void
NativeRegExpMacroAssembler::JumpOrBacktrack(Label *to)
{
    IonSpew(SPEW_PREFIX "JumpOrBacktrack");

    if (to)
        masm.jump(to);
    else
        Backtrack();
}

bool
NativeRegExpMacroAssembler::CheckSpecialCharacterClass(jschar type, Label* on_no_match)
{
    IonSpew(SPEW_PREFIX "CheckSpecialCharacterClass(%d)", (int) type);

    // Range checks (c in min..max) are generally implemented by an unsigned
    // (c - min) <= (max - min) check
    switch (type) {
      case 's':
        // Match space-characters
        if (mode_ == ASCII)
            MOZ_ASSUME_UNREACHABLE("Ascii version not implemented");
        return false;
      case 'S':
        // The emitted code for generic character classes is good enough.
        return false;
      case 'd':
        // Match ASCII digits ('0'..'9')
        masm.computeEffectiveAddress(Address(current_character, -'0'), temp0);
        masm.cmp32(temp0, Imm32('9' - '0'));
        BranchOrBacktrack(Assembler::Above, on_no_match);
        return true;
      case 'D':
        // Match non ASCII-digits
        masm.computeEffectiveAddress(Address(current_character, -'0'), temp0);
        masm.cmp32(temp0, Imm32('9' - '0'));
        BranchOrBacktrack(Assembler::BelowOrEqual, on_no_match);
        return true;
      case '.': {
        // Match non-newlines (not 0x0a('\n'), 0x0d('\r'), 0x2028 and 0x2029)
        masm.mov(current_character, temp0);
        masm.xor32(Imm32(0x01), temp0);

        // See if current character is '\n'^1 or '\r'^1, i.e., 0x0b or 0x0c
        masm.sub32(Imm32(0x0b), temp0);
        masm.cmp32(temp0, Imm32(0x0c - 0x0b));
        BranchOrBacktrack(Assembler::BelowOrEqual, on_no_match);
        if (mode_ == JSCHAR) {
            // Compare original value to 0x2028 and 0x2029, using the already
            // computed (current_char ^ 0x01 - 0x0b). I.e., check for
            // 0x201d (0x2028 - 0x0b) or 0x201e.
            masm.sub32(Imm32(0x2028 - 0x0b), temp0);
            masm.cmp32(temp0, Imm32(0x2029 - 0x2028));
            BranchOrBacktrack(Assembler::BelowOrEqual, on_no_match);
        }
        return true;
      }
      case 'w': {
        if (mode_ != ASCII) {
            // Table is 128 entries, so all ASCII characters can be tested.
            masm.cmp32(current_character, Imm32('z'));
            BranchOrBacktrack(Assembler::Above, on_no_match);
        }
        JS_ASSERT(0 == word_character_map[0]);  // Character '\0' is not a word char.
        masm.mov(ImmPtr(word_character_map), temp0);
        masm.load8ZeroExtend(BaseIndex(temp0, current_character, TimesOne), temp0);
        masm.test32(temp0, temp0);
        BranchOrBacktrack(Assembler::Zero, on_no_match);
        return true;
      }
      case 'W': {
        Label done;
        if (mode_ != ASCII) {
            // Table is 128 entries, so all ASCII characters can be tested.
            masm.cmp32(current_character, Imm32('z'));
            masm.j(Assembler::Above, &done);
        }
        JS_ASSERT(0 == word_character_map[0]);  // Character '\0' is not a word char.
        masm.mov(ImmPtr(word_character_map), temp0);
        masm.load8ZeroExtend(BaseIndex(temp0, current_character, TimesOne), temp0);
        masm.test32(temp0, temp0);
        BranchOrBacktrack(Assembler::NonZero, on_no_match);
        if (mode_ != ASCII)
            masm.bind(&done);
        return true;
      }
        // Non-standard classes (with no syntactic shorthand) used internally.
      case '*':
        // Match any character.
        return true;
      case 'n': {
        // Match newlines (0x0a('\n'), 0x0d('\r'), 0x2028 or 0x2029).
        // The opposite of '.'.
        masm.mov(current_character, temp0);
        masm.xor32(Imm32(0x01), temp0);

        // See if current character is '\n'^1 or '\r'^1, i.e., 0x0b or 0x0c
        masm.sub32(Imm32(0x0b), temp0);
        masm.cmp32(temp0, Imm32(0x0c - 0x0b));

        if (mode_ == ASCII) {
            BranchOrBacktrack(Assembler::Above, on_no_match);
        } else {
            Label done;
            BranchOrBacktrack(Assembler::BelowOrEqual, &done);
            JS_ASSERT(JSCHAR == mode_);

            // Compare original value to 0x2028 and 0x2029, using the already
            // computed (current_char ^ 0x01 - 0x0b). I.e., check for
            // 0x201d (0x2028 - 0x0b) or 0x201e.
            masm.sub32(Imm32(0x2028 - 0x0b), temp0);
            masm.cmp32(temp0, Imm32(1));
            BranchOrBacktrack(Assembler::Above, on_no_match);

            masm.bind(&done);
        }
        return true;
      }
        // No custom implementation (yet):
      default:
        return false;
    }
}

bool
NativeRegExpMacroAssembler::CanReadUnaligned()
{
    return true;
}

const uint8_t
NativeRegExpMacroAssembler::word_character_map[] =
{
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,

    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // '0' - '7'
    0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,  // '8' - '9'

    0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'A' - 'G'
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'H' - 'O'
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'P' - 'W'
    0xffu, 0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu,  // 'X' - 'Z', '_'

    0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'a' - 'g'
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'h' - 'o'
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // 'p' - 'w'
    0xffu, 0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,  // 'x' - 'z'

    // Latin-1 range
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,

    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,

    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,

    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
};
