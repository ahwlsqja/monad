// Copyright (C) 2025-26 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifdef MONAD_ZKVM

    #pragma once

    #if defined(MONAD_ZKVM_SP1)
extern "C" [[noreturn]] void syscall_halt(unsigned char exit_code);
    #endif

namespace monad::zkvm
{
    [[noreturn]] inline void exit(int status)
    {
    #if defined(MONAD_ZKVM_ZISK)
        // ZisK: ecall with syscall number 93
        register int a0 asm("a0") = status;
        register int a7 asm("a7") = 93;
        asm volatile("ecall" : : "r"(a0), "r"(a7));
        __builtin_unreachable();
    #elif defined(MONAD_ZKVM_SP1)
        syscall_halt(static_cast<unsigned char>(status));
    #else
        #error                                                                 \
            "No zkVM exit backend defined (expected MONAD_ZKVM_ZISK or MONAD_ZKVM_SP1)"
    #endif
    }
}

#endif
