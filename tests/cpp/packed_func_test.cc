/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <dmlc/logging.h>
#include <gtest/gtest.h>
#include <tvm/runtime/packed_func.h>
#include <tvm/packed_func_ext.h>
#include <tvm/ir.h>

TEST(PackedFunc, Basic) {
  using namespace tvm;
  using namespace tvm::runtime;
  int x = 0;
  void* handle = &x;
  TVMArray a;

  Var v = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      CHECK(args.num_args == 3);
      CHECK(args.values[0].v_float64 == 1.0);
      CHECK(args.type_codes[0] == kDLFloat);
      CHECK(args.values[1].v_handle == &a);
      CHECK(args.type_codes[1] == kArrayHandle);
      CHECK(args.values[2].v_handle == &x);
      CHECK(args.type_codes[2] == kHandle);
      *rv = Var("a");
    })(1.0, &a, handle);
  CHECK(v->name_hint == "a");
}

TEST(PackedFunc, Node) {
  using namespace tvm;
  using namespace tvm::runtime;
  Var x;
  Var t = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      CHECK(args.num_args == 1);
      CHECK(args.type_codes[0] == kNodeHandle);
      Var b = args[0];
      CHECK(x.same_as(b));
      *rv = b;
    })(x);
  CHECK(t.same_as(x));
}

TEST(PackedFunc, NDArray) {
  using namespace tvm;
  using namespace tvm::runtime;
  auto x = NDArray::Empty(
      {}, String2TVMType("float32"),
      TVMContext{kDLCPU, 0});
  reinterpret_cast<float*>(x->data)[0] = 10.0f;
  CHECK(x.use_count() == 1);

  PackedFunc forward([&](TVMArgs args, TVMRetValue* rv) {
      *rv = args[0];
    });

  NDArray ret = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      NDArray y = args[0];
      DLTensor* ptr = args[0];
      CHECK(ptr == x.operator->());
      CHECK(x.same_as(y));
      CHECK(x.use_count() == 2);
      *rv = forward(y);
    })(x);
  CHECK(ret.use_count() == 2);
  CHECK(ret.same_as(x));
}

TEST(PackedFunc, str) {
  using namespace tvm;
  using namespace tvm::runtime;
  PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      CHECK(args.num_args == 1);
      std::string x = args[0];
      CHECK(x == "hello");
      *rv = x;
    })("hello");
}


TEST(PackedFunc, func) {
  using namespace tvm;
  using namespace tvm::runtime;
  PackedFunc addone([&](TVMArgs args, TVMRetValue* rv) {
      *rv = args[0].operator int() + 1;
    });
  // function as arguments
  int r0 = PackedFunc([](TVMArgs args, TVMRetValue* rv) {
      PackedFunc f = args[0];
      // TVMArgValue -> Arguments as function
      *rv = f(args[1]).operator int();
    })(addone, 1);
  CHECK_EQ(r0, 2);

  int r1 = PackedFunc([](TVMArgs args, TVMRetValue* rv) {
      // TVMArgValue -> TVMRetValue
      *rv = args[1];
    })(2, 100);
  CHECK_EQ(r1, 100);

  int r2 = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      // re-assignment
      *rv = args[0];
      // TVMRetValue -> Function argument
      *rv = addone(args[0].operator PackedFunc()(args[1], 1));
    })(addone, 100);
  CHECK_EQ(r2, 102);
}

TEST(PackedFunc, Expr) {
  using namespace tvm;
  using namespace tvm::runtime;
  // automatic conversion of int to expr
  PackedFunc addone([](TVMArgs args, TVMRetValue* rv) {
      Expr x = args[0];
      *rv = x.as<tvm::ir::IntImm>()->value + 1;
  });
  int r0 = PackedFunc([](TVMArgs args, TVMRetValue* rv) {
      PackedFunc f = args[0];
      // TVMArgValue -> Arguments as function
      *rv = f(args[1]).operator int();
    })(addone, 1);
  CHECK_EQ(r0, 2);
}

TEST(PackedFunc, Type) {
  using namespace tvm;
  using namespace tvm::runtime;
  auto get_type = PackedFunc([](TVMArgs args, TVMRetValue* rv) {
      Type x = args[0];
      *rv = x;
    });
  auto get_type2 = PackedFunc([](TVMArgs args, TVMRetValue* rv) {
      *rv = args[0];
    });
  CHECK(get_type("int32").operator Type() == Int(32));
  CHECK(get_type("float").operator Type() == Float(32));
  CHECK(get_type2("float32x2").operator Type() == Float(32, 2));
}

TEST(TypedPackedFunc, HighOrder) {
  using namespace tvm;
  using namespace tvm::runtime;
  using Int1Func = TypedPackedFunc<int(int)>;
  using Int2Func = TypedPackedFunc<int(int, int)>;
  using BindFunc = TypedPackedFunc<Int1Func(Int2Func, int value)>;
  BindFunc ftyped;
  ftyped = [](Int2Func f1, int value) -> Int1Func {
    auto binded = [f1, value](int x) {
      return f1(value, x);
    };
    Int1Func x(binded);
    return x;
  };
  auto add = [](int x, int y) { return x + y; };
  CHECK_EQ(ftyped(Int2Func(add), 1)(2), 3);
  PackedFunc f = ftyped(Int2Func(add), 1);
  CHECK_EQ(f(3).operator int(), 4);
  // call the type erased version.
  Int1Func f1 = ftyped.packed()(Int2Func(add), 1);
  CHECK_EQ(f1(3), 4);
}

// new namespoace
namespace test {
// register int vector as extension type
using IntVector = std::vector<int>;
}  // namespace test

namespace tvm {
namespace runtime {

template<>
struct extension_type_info<test::IntVector> {
  static const int code = kExtBegin + 1;
};
}  // runtime
}  // tvm

// do registration, this need to be in cc file
TVM_REGISTER_EXT_TYPE(test::IntVector);

TEST(PackedFunc, ExtensionType) {
  using namespace tvm;
  using namespace tvm::runtime;
  // note: class are copy by value.
  test::IntVector vec{1, 2, 4};

  auto copy_vec = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      // copy by value
      const test::IntVector& v = args[0].AsExtension<test::IntVector>();
      CHECK(&v == &vec);
      test::IntVector v2 = args[0];
      CHECK_EQ(v2.size(), 3U);
      CHECK_EQ(v[2], 4);
      // return copy by value
      *rv = v2;
    });

  auto pass_vec = PackedFunc([&](TVMArgs args, TVMRetValue* rv) {
      // copy by value
      *rv = args[0];
    });

  test::IntVector vret1 = copy_vec(vec);
  test::IntVector vret2 = pass_vec(copy_vec(vec));
  CHECK_EQ(vret1.size(), 3U);
  CHECK_EQ(vret2.size(), 3U);
  CHECK_EQ(vret1[2], 4);
  CHECK_EQ(vret2[2], 4);
}


int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  return RUN_ALL_TESTS();
}
