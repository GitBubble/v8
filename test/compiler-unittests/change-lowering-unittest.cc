// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/change-lowering.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/node-properties-inl.h"
#include "src/compiler/simplified-operator.h"
#include "src/compiler/typer.h"
#include "test/compiler-unittests/graph-unittest.h"
#include "testing/gmock-support.h"

using testing::_;
using testing::AllOf;
using testing::Capture;
using testing::CaptureEq;

namespace v8 {
namespace internal {
namespace compiler {

template <typename T>
class ChangeLoweringTest : public GraphTest {
 public:
  static const size_t kPointerSize = sizeof(T);
  static const MachineType kWordRepresentation =
      (kPointerSize == 4) ? kRepWord32 : kRepWord64;
  STATIC_ASSERT(HeapNumber::kValueOffset % kApiPointerSize == 0);
  static const int kHeapNumberValueOffset = static_cast<int>(
      (HeapNumber::kValueOffset / kApiPointerSize) * kPointerSize);

  ChangeLoweringTest() : simplified_(zone()) {}
  virtual ~ChangeLoweringTest() {}

 protected:
  Node* Parameter(int32_t index = 0) {
    return graph()->NewNode(common()->Parameter(index), graph()->start());
  }

  Reduction Reduce(Node* node) {
    Typer typer(zone());
    JSGraph jsgraph(graph(), common(), &typer);
    CompilationInfo info(isolate(), zone());
    Linkage linkage(&info);
    MachineOperatorBuilder machine(zone(), kWordRepresentation);
    ChangeLowering reducer(&jsgraph, &linkage, &machine);
    return reducer.Reduce(node);
  }

  SimplifiedOperatorBuilder* simplified() { return &simplified_; }

  PrintableUnique<HeapObject> true_unique() {
    return PrintableUnique<HeapObject>::CreateImmovable(
        zone(), factory()->true_value());
  }
  PrintableUnique<HeapObject> false_unique() {
    return PrintableUnique<HeapObject>::CreateImmovable(
        zone(), factory()->false_value());
  }

 private:
  SimplifiedOperatorBuilder simplified_;
};


typedef ::testing::Types<int32_t, int64_t> ChangeLoweringTypes;
TYPED_TEST_CASE(ChangeLoweringTest, ChangeLoweringTypes);


TARGET_TYPED_TEST(ChangeLoweringTest, ChangeBitToBool) {
  Node* val = this->Parameter(0);
  Node* node =
      this->graph()->NewNode(this->simplified()->ChangeBitToBool(), val);
  Reduction reduction = this->Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  Node* phi = reduction.replacement();
  Capture<Node*> branch;
  EXPECT_THAT(
      phi, IsPhi(IsHeapConstant(this->true_unique()),
                 IsHeapConstant(this->false_unique()),
                 IsMerge(IsIfTrue(AllOf(CaptureEq(&branch),
                                        IsBranch(val, this->graph()->start()))),
                         IsIfFalse(CaptureEq(&branch)))));
}


TARGET_TYPED_TEST(ChangeLoweringTest, StringAdd) {
  Node* node = this->graph()->NewNode(this->simplified()->StringAdd(),
                                      this->Parameter(0), this->Parameter(1));
  Reduction reduction = this->Reduce(node);
  EXPECT_FALSE(reduction.Changed());
}


class ChangeLowering32Test : public ChangeLoweringTest<int32_t> {
 public:
  virtual ~ChangeLowering32Test() {}
};


TARGET_TEST_F(ChangeLowering32Test, ChangeBoolToBit) {
  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeBoolToBit(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  EXPECT_THAT(reduction.replacement(),
              IsWord32Equal(val, IsHeapConstant(true_unique())));
}


TARGET_TEST_F(ChangeLowering32Test, ChangeInt32ToTagged) {
  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeInt32ToTagged(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  Node* phi = reduction.replacement();
  ASSERT_EQ(IrOpcode::kPhi, phi->opcode());

  Node* smi = NodeProperties::GetValueInput(phi, 1);
  ASSERT_THAT(smi, IsProjection(0, IsInt32AddWithOverflow(val, val)));

  Node* heap_number = NodeProperties::GetValueInput(phi, 0);
  ASSERT_EQ(IrOpcode::kCall, heap_number->opcode());

  Node* merge = NodeProperties::GetControlInput(phi);
  ASSERT_EQ(IrOpcode::kMerge, merge->opcode());

  const int32_t kValueOffset = kHeapNumberValueOffset - kHeapObjectTag;
  EXPECT_THAT(NodeProperties::GetControlInput(merge, 0),
              IsStore(kMachFloat64, kNoWriteBarrier, heap_number,
                      IsInt32Constant(kValueOffset),
                      IsChangeInt32ToFloat64(val), _, heap_number));

  Node* if_true = NodeProperties::GetControlInput(heap_number);
  ASSERT_EQ(IrOpcode::kIfTrue, if_true->opcode());

  Node* if_false = NodeProperties::GetControlInput(merge, 1);
  ASSERT_EQ(IrOpcode::kIfFalse, if_false->opcode());

  Node* branch = NodeProperties::GetControlInput(if_true);
  EXPECT_EQ(branch, NodeProperties::GetControlInput(if_false));
  EXPECT_THAT(branch,
              IsBranch(IsProjection(1, IsInt32AddWithOverflow(val, val)),
                       graph()->start()));
}


TARGET_TEST_F(ChangeLowering32Test, ChangeTaggedToFloat64) {
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize == 1);

  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeTaggedToFloat64(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  const int32_t kShiftAmount =
      kSmiTagSize + SmiTagging<kPointerSize>::kSmiShiftSize;
  const int32_t kValueOffset = kHeapNumberValueOffset - kHeapObjectTag;
  Node* phi = reduction.replacement();
  Capture<Node*> branch;
  EXPECT_THAT(
      phi,
      IsPhi(IsLoad(kMachFloat64, val, IsInt32Constant(kValueOffset), _),
            IsChangeInt32ToFloat64(
                IsWord32Sar(val, IsInt32Constant(kShiftAmount))),
            IsMerge(IsIfTrue(AllOf(
                        CaptureEq(&branch),
                        IsBranch(IsWord32And(val, IsInt32Constant(kSmiTagMask)),
                                 graph()->start()))),
                    IsIfFalse(CaptureEq(&branch)))));
}


class ChangeLowering64Test : public ChangeLoweringTest<int64_t> {
 public:
  virtual ~ChangeLowering64Test() {}
};


TARGET_TEST_F(ChangeLowering64Test, ChangeBoolToBit) {
  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeBoolToBit(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  EXPECT_THAT(reduction.replacement(),
              IsWord64Equal(val, IsHeapConstant(true_unique())));
}


TARGET_TEST_F(ChangeLowering64Test, ChangeInt32ToTagged) {
  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeInt32ToTagged(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  const int32_t kShiftAmount =
      kSmiTagSize + SmiTagging<kPointerSize>::kSmiShiftSize;
  EXPECT_THAT(reduction.replacement(),
              IsWord64Shl(val, IsInt32Constant(kShiftAmount)));
}


TARGET_TEST_F(ChangeLowering64Test, ChangeTaggedToFloat64) {
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize == 1);

  Node* val = Parameter(0);
  Node* node = graph()->NewNode(simplified()->ChangeTaggedToFloat64(), val);
  Reduction reduction = Reduce(node);
  ASSERT_TRUE(reduction.Changed());

  const int32_t kShiftAmount =
      kSmiTagSize + SmiTagging<kPointerSize>::kSmiShiftSize;
  const int32_t kValueOffset = kHeapNumberValueOffset - kHeapObjectTag;
  Node* phi = reduction.replacement();
  Capture<Node*> branch;
  EXPECT_THAT(
      phi,
      IsPhi(IsLoad(kMachFloat64, val, IsInt32Constant(kValueOffset), _),
            IsChangeInt32ToFloat64(IsConvertInt64ToInt32(
                IsWord64Sar(val, IsInt32Constant(kShiftAmount)))),
            IsMerge(IsIfTrue(AllOf(
                        CaptureEq(&branch),
                        IsBranch(IsWord64And(val, IsInt32Constant(kSmiTagMask)),
                                 graph()->start()))),
                    IsIfFalse(CaptureEq(&branch)))));
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
