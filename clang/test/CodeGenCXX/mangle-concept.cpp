// RUN: %clang_cc1 -verify -Wno-return-type -Wno-main -std=c++2a -fconcepts-ts -emit-llvm -triple %itanium_abi_triple -o - %s | FileCheck %s
// expected-no-diagnostics

namespace test1 {
template <bool> struct S {};
template <typename> concept C = true;
template <typename T = int> S<C<T>> f0() { return S<C<T>>{}; }
template S<C<int>> f0<>();
// CHECK: void @_ZN5test12f0IiEENS_1SIXL_ZNS_1CIT_EEEEEEv()
}

template <bool> struct S {};
template <typename> concept C = true;
template <typename T = int> S<C<T>> f0() { return S<C<T>>{}; }
template S<C<int>> f0<>();
// CHECK: void @_Z2f0IiE1SIXL_Z1CIT_EEEEv()