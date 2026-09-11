/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <mutex>

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/common/spin_lock.h"

namespace Dynarmic {

using Backend::Arm64::Wscratch0;
using Backend::Arm64::Wscratch1;
using namespace oaknut::util;

void EmitSpinLockLock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    oaknut::Label start, loop;

    code.MOV(Wscratch1, 1);
    code.SEVL();
    code.l(start);
    code.WFE();
    code.l(loop);
    code.LDAXR(Wscratch0, ptr);
    code.CBNZ(Wscratch0, start);
    code.STXR(Wscratch0, Wscratch1, ptr);
    code.CBNZ(Wscratch0, loop);
}

void EmitSpinLockUnlock(oaknut::CodeGenerator& code, oaknut::XReg ptr) {
    code.STLR(WZR, ptr);
}

namespace {

struct SpinLockImpl {
    SpinLockImpl();

    void Initialize();

    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    void (*lock)(volatile int*);
    void (*unlock)(volatile int*);
};

std::once_flag flag;

// Constructed on first use rather than at namespace scope. SpinLockImpl's constructor
// allocates an oaknut::CodeBlock, which mmaps executable memory - as a global that ran
// during static initialisation, i.e. at dyld load, before main() and before any UI could
// exist. On a platform where executable memory has to be granted (iOS without StikDebug or
// the JIT entitlement) that allocation fails, and failing there means the process dies
// before it can tell anyone why. Deferring it to first use moves the failure to a point
// where the application is running and can report it.
//
// Function-local statics are initialised thread-safely since C++11, so this does not need
// its own guard, and it also removes a static-initialisation-order dependency.
SpinLockImpl& Impl() noexcept {
    static SpinLockImpl impl;
    return impl;
}

SpinLockImpl::SpinLockImpl()
        : mem{4096}
        , code{mem.ptr(), mem.ptr()} {}

void SpinLockImpl::Initialize() {
    mem.unprotect();

    lock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockLock(code, X0);
    code.RET();

    unlock = code.xptr<void (*)(volatile int*)>();
    EmitSpinLockUnlock(code, X0);
    code.RET();

    mem.protect();
    mem.invalidate_all();
}

}  // namespace

void SpinLock::Lock() noexcept {
    SpinLockImpl& impl = Impl();
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.lock(&storage);
}

void SpinLock::Unlock() noexcept {
    SpinLockImpl& impl = Impl();
    std::call_once(flag, &SpinLockImpl::Initialize, impl);
    impl.unlock(&storage);
}

}  // namespace Dynarmic
