//
// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/boot_control_android.h"

#include <set>
#include <vector>

#include <base/logging.h>
#include <base/strings/string_util.h>
#include <fs_mgr.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "update_engine/mock_boot_control_hal.h"
#include "update_engine/mock_dynamic_partition_control.h"

using android::fs_mgr::MetadataBuilder;
using android::hardware::Void;
using testing::_;
using testing::AnyNumber;
using testing::Contains;
using testing::Eq;
using testing::Invoke;
using testing::Key;
using testing::MakeMatcher;
using testing::Matcher;
using testing::MatcherInterface;
using testing::MatchResultListener;
using testing::NiceMock;
using testing::Not;
using testing::Return;

namespace chromeos_update_engine {

constexpr const uint32_t kMaxNumSlots = 2;
constexpr const char* kSlotSuffixes[kMaxNumSlots] = {"_a", "_b"};
constexpr const char* kFakeDevicePath = "/fake/dev/path/";
constexpr const char* kFakeMappedPath = "/fake/mapped/path/";
constexpr const uint32_t kFakeMetadataSize = 65536;
constexpr const char* kDefaultGroup = "foo";

// "vendor"
struct PartitionName : std::string {
  using std::string::string;
};

// "vendor_a"
struct PartitionNameSuffix : std::string {
  using std::string::string;
};

// A map describing the size of each partition.
using PartitionSizes = std::map<PartitionName, uint64_t>;
using PartitionSuffixSizes = std::map<PartitionNameSuffix, uint64_t>;

using PartitionMetadata = BootControlInterface::PartitionMetadata;

// C++ standards do not allow uint64_t (aka unsigned long) to be the parameter
// of user-defined literal operators.
constexpr unsigned long long operator"" _MiB(unsigned long long x) {  // NOLINT
  return x << 20;
}
constexpr unsigned long long operator"" _GiB(unsigned long long x) {  // NOLINT
  return x << 30;
}

constexpr uint64_t kDefaultGroupSize = 5_GiB;
// Super device size. 1 MiB for metadata.
constexpr uint64_t kDefaultSuperSize = kDefaultGroupSize * 2 + 1_MiB;

template <typename U, typename V>
std::ostream& operator<<(std::ostream& os, const std::map<U, V>& param) {
  os << "{";
  bool first = true;
  for (const auto& pair : param) {
    if (!first)
      os << ", ";
    os << pair.first << ":" << pair.second;
    first = false;
  }
  return os << "}";
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& param) {
  os << "[";
  bool first = true;
  for (const auto& e : param) {
    if (!first)
      os << ", ";
    os << e;
    first = false;
  }
  return os << "]";
}

std::ostream& operator<<(std::ostream& os,
                         const PartitionMetadata::Partition& p) {
  return os << "{" << p.name << ", " << p.size << "}";
}

std::ostream& operator<<(std::ostream& os, const PartitionMetadata::Group& g) {
  return os << "{" << g.name << ", " << g.size << ", " << g.partitions << "}";
}

std::ostream& operator<<(std::ostream& os, const PartitionMetadata& m) {
  return os << m.groups;
}

inline std::string GetDevice(const std::string& name) {
  return kFakeDevicePath + name;
}
inline std::string GetSuperDevice() {
  return GetDevice(fs_mgr_get_super_partition_name());
}

struct TestParam {
  uint32_t source;
  uint32_t target;
};
std::ostream& operator<<(std::ostream& os, const TestParam& param) {
  return os << "{source: " << param.source << ", target:" << param.target
            << "}";
}

// To support legacy tests, auto-convert {name_a: size} map to
// PartitionMetadata.
PartitionMetadata toMetadata(const PartitionSuffixSizes& partition_sizes) {
  PartitionMetadata metadata;
  for (const char* suffix : kSlotSuffixes) {
    metadata.groups.push_back(
        {std::string(kDefaultGroup) + suffix, kDefaultGroupSize, {}});
  }
  for (const auto& pair : partition_sizes) {
    for (size_t suffix_idx = 0; suffix_idx < kMaxNumSlots; ++suffix_idx) {
      if (base::EndsWith(pair.first,
                         kSlotSuffixes[suffix_idx],
                         base::CompareCase::SENSITIVE)) {
        metadata.groups[suffix_idx].partitions.push_back(
            {pair.first, pair.second});
      }
    }
  }
  return metadata;
}

// To support legacy tests, auto-convert {name: size} map to PartitionMetadata.
PartitionMetadata toMetadata(const PartitionSizes& partition_sizes) {
  PartitionMetadata metadata;
  metadata.groups.push_back(
      {std::string{kDefaultGroup}, kDefaultGroupSize, {}});
  for (const auto& pair : partition_sizes) {
    metadata.groups[0].partitions.push_back({pair.first, pair.second});
  }
  return metadata;
}

std::unique_ptr<MetadataBuilder> NewFakeMetadata(
    const PartitionMetadata& metadata) {
  auto builder =
      MetadataBuilder::New(kDefaultSuperSize, kFakeMetadataSize, kMaxNumSlots);
  EXPECT_GE(builder->AllocatableSpace(), kDefaultGroupSize * 2);
  EXPECT_NE(nullptr, builder);
  if (builder == nullptr)
    return nullptr;
  for (const auto& group : metadata.groups) {
    EXPECT_TRUE(builder->AddGroup(group.name, group.size));
    for (const auto& partition : group.partitions) {
      auto p = builder->AddPartition(partition.name, group.name, 0 /* attr */);
      EXPECT_TRUE(p && builder->ResizePartition(p, partition.size));
    }
  }
  return builder;
}

class MetadataMatcher : public MatcherInterface<MetadataBuilder*> {
 public:
  explicit MetadataMatcher(const PartitionSuffixSizes& partition_sizes)
      : partition_metadata_(toMetadata(partition_sizes)) {}
  explicit MetadataMatcher(const PartitionMetadata& partition_metadata)
      : partition_metadata_(partition_metadata) {}

  bool MatchAndExplain(MetadataBuilder* metadata,
                       MatchResultListener* listener) const override {
    bool success = true;
    for (const auto& group : partition_metadata_.groups) {
      for (const auto& partition : group.partitions) {
        auto p = metadata->FindPartition(partition.name);
        if (p == nullptr) {
          if (!success)
            *listener << "; ";
          *listener << "No partition " << partition.name;
          success = false;
          continue;
        }
        if (p->size() != partition.size) {
          if (!success)
            *listener << "; ";
          *listener << "Partition " << partition.name << " has size "
                    << p->size() << ", expected " << partition.size;
          success = false;
        }
        if (p->group_name() != group.name) {
          if (!success)
            *listener << "; ";
          *listener << "Partition " << partition.name << " has group "
                    << p->group_name() << ", expected " << group.name;
          success = false;
        }
      }
    }
    return success;
  }

  void DescribeTo(std::ostream* os) const override {
    *os << "expect: " << partition_metadata_;
  }

  void DescribeNegationTo(std::ostream* os) const override {
    *os << "expect not: " << partition_metadata_;
  }

 private:
  PartitionMetadata partition_metadata_;
};

inline Matcher<MetadataBuilder*> MetadataMatches(
    const PartitionSuffixSizes& partition_sizes) {
  return MakeMatcher(new MetadataMatcher(partition_sizes));
}

inline Matcher<MetadataBuilder*> MetadataMatches(
    const PartitionMetadata& partition_metadata) {
  return MakeMatcher(new MetadataMatcher(partition_metadata));
}

MATCHER_P(HasGroup, group, " has group " + group) {
  auto groups = arg->ListGroups();
  return std::find(groups.begin(), groups.end(), group) != groups.end();
}

class BootControlAndroidTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Fake init bootctl_
    bootctl_.module_ = new NiceMock<MockBootControlHal>();
    bootctl_.dynamic_control_ =
        std::make_unique<NiceMock<MockDynamicPartitionControl>>();

    ON_CALL(module(), getNumberSlots()).WillByDefault(Invoke([] {
      return kMaxNumSlots;
    }));
    ON_CALL(module(), getSuffix(_, _))
        .WillByDefault(Invoke([](auto slot, auto cb) {
          EXPECT_LE(slot, kMaxNumSlots);
          cb(slot < kMaxNumSlots ? kSlotSuffixes[slot] : "");
          return Void();
        }));

    ON_CALL(dynamicControl(), IsDynamicPartitionsEnabled())
        .WillByDefault(Return(true));
    ON_CALL(dynamicControl(), GetDeviceDir(_))
        .WillByDefault(Invoke([](auto path) {
          *path = kFakeDevicePath;
          return true;
        }));
  }

  // Return the mocked HAL module.
  NiceMock<MockBootControlHal>& module() {
    return static_cast<NiceMock<MockBootControlHal>&>(*bootctl_.module_);
  }

  // Return the mocked DynamicPartitionControlInterface.
  NiceMock<MockDynamicPartitionControl>& dynamicControl() {
    return static_cast<NiceMock<MockDynamicPartitionControl>&>(
        *bootctl_.dynamic_control_);
  }

  // Set the fake metadata to return when LoadMetadataBuilder is called on
  // |slot|.
  void SetMetadata(uint32_t slot, const PartitionSuffixSizes& sizes) {
    SetMetadata(slot, toMetadata(sizes));
  }

  void SetMetadata(uint32_t slot, const PartitionMetadata& metadata) {
    EXPECT_CALL(dynamicControl(),
                LoadMetadataBuilder(GetSuperDevice(), slot, _))
        .Times(AnyNumber())
        .WillRepeatedly(Invoke([metadata](auto, auto, auto) {
          return NewFakeMetadata(metadata);
        }));
  }

  // Expect that MapPartitionOnDeviceMapper is called on target() metadata slot
  // with each partition in |partitions|.
  void ExpectMap(const std::set<std::string>& partitions,
                 bool force_writable = true) {
    // Error when MapPartitionOnDeviceMapper is called on unknown arguments.
    ON_CALL(dynamicControl(), MapPartitionOnDeviceMapper(_, _, _, _, _))
        .WillByDefault(Return(false));

    for (const auto& partition : partitions) {
      EXPECT_CALL(dynamicControl(),
                  MapPartitionOnDeviceMapper(
                      GetSuperDevice(), partition, target(), force_writable, _))
          .WillOnce(Invoke([this](auto, auto partition, auto, auto, auto path) {
            auto it = mapped_devices_.find(partition);
            if (it != mapped_devices_.end()) {
              *path = it->second;
              return true;
            }
            mapped_devices_[partition] = *path = kFakeMappedPath + partition;
            return true;
          }));
    }
  }

  // Expect that UnmapPartitionOnDeviceMapper is called on target() metadata
  // slot with each partition in |partitions|.
  void ExpectUnmap(const std::set<std::string>& partitions) {
    // Error when UnmapPartitionOnDeviceMapper is called on unknown arguments.
    ON_CALL(dynamicControl(), UnmapPartitionOnDeviceMapper(_, _))
        .WillByDefault(Return(false));

    for (const auto& partition : partitions) {
      EXPECT_CALL(dynamicControl(), UnmapPartitionOnDeviceMapper(partition, _))
          .WillOnce(Invoke([this](auto partition, auto) {
            mapped_devices_.erase(partition);
            return true;
          }));
    }
  }

  void ExpectRemap(const std::set<std::string>& partitions) {
    ExpectUnmap(partitions);
    ExpectMap(partitions);
  }

  void ExpectDevicesAreMapped(const std::set<std::string>& partitions) {
    ASSERT_EQ(partitions.size(), mapped_devices_.size());
    for (const auto& partition : partitions) {
      EXPECT_THAT(mapped_devices_, Contains(Key(Eq(partition))))
          << "Expect that " << partition << " is mapped, but it is not.";
    }
  }

  void ExpectStoreMetadata(const PartitionSuffixSizes& partition_sizes) {
    ExpectStoreMetadataMatch(MetadataMatches(partition_sizes));
  }

  virtual void ExpectStoreMetadataMatch(
      const Matcher<MetadataBuilder*>& matcher) {
    EXPECT_CALL(dynamicControl(),
                StoreMetadata(GetSuperDevice(), matcher, target()))
        .WillOnce(Return(true));
  }

  uint32_t source() { return slots_.source; }

  uint32_t target() { return slots_.target; }

  // Return partition names with suffix of source().
  PartitionNameSuffix S(const std::string& name) {
    return PartitionNameSuffix(name + std::string(kSlotSuffixes[source()]));
  }

  // Return partition names with suffix of target().
  PartitionNameSuffix T(const std::string& name) {
    return PartitionNameSuffix(name + std::string(kSlotSuffixes[target()]));
  }

  // Set source and target slots to use before testing.
  void SetSlots(const TestParam& slots) {
    slots_ = slots;

    ON_CALL(module(), getCurrentSlot()).WillByDefault(Invoke([this] {
      return source();
    }));
    // Should not store metadata to source slot.
    EXPECT_CALL(dynamicControl(), StoreMetadata(GetSuperDevice(), _, source()))
        .Times(0);
    // Should not load metadata from target slot.
    EXPECT_CALL(dynamicControl(),
                LoadMetadataBuilder(GetSuperDevice(), target(), _))
        .Times(0);
  }

  bool InitPartitionMetadata(uint32_t slot, PartitionSizes partition_sizes) {
    auto m = toMetadata(partition_sizes);
    LOG(INFO) << m;
    return bootctl_.InitPartitionMetadata(slot, m);
  }

  BootControlAndroid bootctl_;  // BootControlAndroid under test.
  TestParam slots_;
  // mapped devices through MapPartitionOnDeviceMapper.
  std::map<std::string, std::string> mapped_devices_;
};

class BootControlAndroidTestP
    : public BootControlAndroidTest,
      public ::testing::WithParamInterface<TestParam> {
 public:
  void SetUp() override {
    BootControlAndroidTest::SetUp();
    SetSlots(GetParam());
  }
};

// Test resize case. Grow if target metadata contains a partition with a size
// less than expected.
TEST_P(BootControlAndroidTestP, NeedGrowIfSizeNotMatchWhenResizing) {
  SetMetadata(source(),
              {{S("system"), 2_GiB},
               {S("vendor"), 1_GiB},
               {T("system"), 2_GiB},
               {T("vendor"), 1_GiB}});
  ExpectStoreMetadata({{S("system"), 2_GiB},
                       {S("vendor"), 1_GiB},
                       {T("system"), 3_GiB},
                       {T("vendor"), 1_GiB}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(
      InitPartitionMetadata(target(), {{"system", 3_GiB}, {"vendor", 1_GiB}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

// Test resize case. Shrink if target metadata contains a partition with a size
// greater than expected.
TEST_P(BootControlAndroidTestP, NeedShrinkIfSizeNotMatchWhenResizing) {
  SetMetadata(source(),
              {{S("system"), 2_GiB},
               {S("vendor"), 1_GiB},
               {T("system"), 2_GiB},
               {T("vendor"), 1_GiB}});
  ExpectStoreMetadata({{S("system"), 2_GiB},
                       {S("vendor"), 1_GiB},
                       {T("system"), 2_GiB},
                       {T("vendor"), 150_MiB}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(InitPartitionMetadata(target(),
                                    {{"system", 2_GiB}, {"vendor", 150_MiB}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

// Test adding partitions on the first run.
TEST_P(BootControlAndroidTestP, AddPartitionToEmptyMetadata) {
  SetMetadata(source(), PartitionSuffixSizes{});
  ExpectStoreMetadata({{T("system"), 2_GiB}, {T("vendor"), 1_GiB}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(
      InitPartitionMetadata(target(), {{"system", 2_GiB}, {"vendor", 1_GiB}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

// Test subsequent add case.
TEST_P(BootControlAndroidTestP, AddAdditionalPartition) {
  SetMetadata(source(), {{S("system"), 2_GiB}, {T("system"), 2_GiB}});
  ExpectStoreMetadata(
      {{S("system"), 2_GiB}, {T("system"), 2_GiB}, {T("vendor"), 1_GiB}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(
      InitPartitionMetadata(target(), {{"system", 2_GiB}, {"vendor", 1_GiB}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

// Test delete one partition.
TEST_P(BootControlAndroidTestP, DeletePartition) {
  SetMetadata(source(),
              {{S("system"), 2_GiB},
               {S("vendor"), 1_GiB},
               {T("system"), 2_GiB},
               {T("vendor"), 1_GiB}});
  // No T("vendor")
  ExpectStoreMetadata(
      {{S("system"), 2_GiB}, {S("vendor"), 1_GiB}, {T("system"), 2_GiB}});
  ExpectRemap({T("system")});

  EXPECT_TRUE(InitPartitionMetadata(target(), {{"system", 2_GiB}}));
  ExpectDevicesAreMapped({T("system")});
}

// Test delete all partitions.
TEST_P(BootControlAndroidTestP, DeleteAll) {
  SetMetadata(source(),
              {{S("system"), 2_GiB},
               {S("vendor"), 1_GiB},
               {T("system"), 2_GiB},
               {T("vendor"), 1_GiB}});
  ExpectStoreMetadata({{S("system"), 2_GiB}, {S("vendor"), 1_GiB}});

  EXPECT_TRUE(InitPartitionMetadata(target(), {}));
  ExpectDevicesAreMapped({});
}

// Test corrupt source metadata case.
TEST_P(BootControlAndroidTestP, CorruptedSourceMetadata) {
  EXPECT_CALL(dynamicControl(),
              LoadMetadataBuilder(GetSuperDevice(), source(), _))
      .WillOnce(Invoke([](auto, auto, auto) { return nullptr; }));
  EXPECT_FALSE(InitPartitionMetadata(target(), {{"system", 1_GiB}}))
      << "Should not be able to continue with corrupt source metadata";
}

// Test that InitPartitionMetadata fail if there is not enough space on the
// device.
TEST_P(BootControlAndroidTestP, NotEnoughSpace) {
  SetMetadata(source(),
              {{S("system"), 3_GiB},
               {S("vendor"), 2_GiB},
               {T("system"), 0},
               {T("vendor"), 0}});
  EXPECT_FALSE(
      InitPartitionMetadata(target(), {{"system", 3_GiB}, {"vendor", 3_GiB}}))
      << "Should not be able to fit 11GiB data into 10GiB space";
}

TEST_P(BootControlAndroidTestP, NotEnoughSpaceForSlot) {
  SetMetadata(source(),
              {{S("system"), 1_GiB},
               {S("vendor"), 1_GiB},
               {T("system"), 0},
               {T("vendor"), 0}});
  EXPECT_FALSE(
      InitPartitionMetadata(target(), {{"system", 3_GiB}, {"vendor", 3_GiB}}))
      << "Should not be able to grow over size of super / 2";
}

INSTANTIATE_TEST_CASE_P(BootControlAndroidTest,
                        BootControlAndroidTestP,
                        testing::Values(TestParam{0, 1}, TestParam{1, 0}));

const PartitionSuffixSizes update_sizes_0() {
  // Initial state is 0 for "other" slot.
  return {
      {"grown_a", 2_GiB},
      {"shrunk_a", 1_GiB},
      {"same_a", 100_MiB},
      {"deleted_a", 150_MiB},
      // no added_a
      {"grown_b", 200_MiB},
      // simulate system_other
      {"shrunk_b", 0},
      {"same_b", 0},
      {"deleted_b", 0},
      // no added_b
  };
}

const PartitionSuffixSizes update_sizes_1() {
  return {
      {"grown_a", 2_GiB},
      {"shrunk_a", 1_GiB},
      {"same_a", 100_MiB},
      {"deleted_a", 150_MiB},
      // no added_a
      {"grown_b", 3_GiB},
      {"shrunk_b", 150_MiB},
      {"same_b", 100_MiB},
      {"added_b", 150_MiB},
      // no deleted_b
  };
}

const PartitionSuffixSizes update_sizes_2() {
  return {
      {"grown_a", 4_GiB},
      {"shrunk_a", 100_MiB},
      {"same_a", 100_MiB},
      {"deleted_a", 64_MiB},
      // no added_a
      {"grown_b", 3_GiB},
      {"shrunk_b", 150_MiB},
      {"same_b", 100_MiB},
      {"added_b", 150_MiB},
      // no deleted_b
  };
}

// Test case for first update after the device is manufactured, in which
// case the "other" slot is likely of size "0" (except system, which is
// non-zero because of system_other partition)
TEST_F(BootControlAndroidTest, SimulatedFirstUpdate) {
  SetSlots({0, 1});

  SetMetadata(source(), update_sizes_0());
  SetMetadata(target(), update_sizes_0());
  ExpectStoreMetadata(update_sizes_1());
  ExpectRemap({"grown_b", "shrunk_b", "same_b", "added_b"});

  EXPECT_TRUE(InitPartitionMetadata(target(),
                                    {{"grown", 3_GiB},
                                     {"shrunk", 150_MiB},
                                     {"same", 100_MiB},
                                     {"added", 150_MiB}}));
  ExpectDevicesAreMapped({"grown_b", "shrunk_b", "same_b", "added_b"});
}

// After first update, test for the second update. In the second update, the
// "added" partition is deleted and "deleted" partition is re-added.
TEST_F(BootControlAndroidTest, SimulatedSecondUpdate) {
  SetSlots({1, 0});

  SetMetadata(source(), update_sizes_1());
  SetMetadata(target(), update_sizes_0());

  ExpectStoreMetadata(update_sizes_2());
  ExpectRemap({"grown_a", "shrunk_a", "same_a", "deleted_a"});

  EXPECT_TRUE(InitPartitionMetadata(target(),
                                    {{"grown", 4_GiB},
                                     {"shrunk", 100_MiB},
                                     {"same", 100_MiB},
                                     {"deleted", 64_MiB}}));
  ExpectDevicesAreMapped({"grown_a", "shrunk_a", "same_a", "deleted_a"});
}

TEST_F(BootControlAndroidTest, ApplyingToCurrentSlot) {
  SetSlots({1, 1});
  EXPECT_FALSE(InitPartitionMetadata(target(), {}))
      << "Should not be able to apply to current slot.";
}

class BootControlAndroidGroupTestP : public BootControlAndroidTestP {
 public:
  void SetUp() override {
    BootControlAndroidTestP::SetUp();
    SetMetadata(
        source(),
        {.groups = {SimpleGroup(S("android"), 3_GiB, S("system"), 2_GiB),
                    SimpleGroup(S("oem"), 2_GiB, S("vendor"), 1_GiB),
                    SimpleGroup(T("android"), 3_GiB, T("system"), 0),
                    SimpleGroup(T("oem"), 2_GiB, T("vendor"), 0)}});
  }

  // Return a simple group with only one partition.
  PartitionMetadata::Group SimpleGroup(const std::string& group,
                                       uint64_t group_size,
                                       const std::string& partition,
                                       uint64_t partition_size) {
    return {.name = group,
            .size = group_size,
            .partitions = {{.name = partition, .size = partition_size}}};
  }

  void ExpectStoreMetadata(const PartitionMetadata& partition_metadata) {
    ExpectStoreMetadataMatch(MetadataMatches(partition_metadata));
  }

  // Expect that target slot is stored with target groups.
  void ExpectStoreMetadataMatch(
      const Matcher<MetadataBuilder*>& matcher) override {
    BootControlAndroidTestP::ExpectStoreMetadataMatch(AllOf(
        MetadataMatches(PartitionMetadata{
            .groups = {SimpleGroup(S("android"), 3_GiB, S("system"), 2_GiB),
                       SimpleGroup(S("oem"), 2_GiB, S("vendor"), 1_GiB)}}),
        matcher));
  }
};

// Allow to resize within group.
TEST_P(BootControlAndroidGroupTestP, ResizeWithinGroup) {
  ExpectStoreMetadata(PartitionMetadata{
      .groups = {SimpleGroup(T("android"), 3_GiB, T("system"), 3_GiB),
                 SimpleGroup(T("oem"), 2_GiB, T("vendor"), 2_GiB)}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {SimpleGroup("android", 3_GiB, "system", 3_GiB),
                     SimpleGroup("oem", 2_GiB, "vendor", 2_GiB)}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

TEST_P(BootControlAndroidGroupTestP, NotEnoughSpaceForGroup) {
  EXPECT_FALSE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {SimpleGroup("android", 3_GiB, "system", 1_GiB),
                     SimpleGroup("oem", 2_GiB, "vendor", 3_GiB)}}))
      << "Should not be able to grow over maximum size of group";
}

TEST_P(BootControlAndroidGroupTestP, GroupTooBig) {
  EXPECT_FALSE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{.groups = {{.name = "android", .size = 3_GiB},
                                   {.name = "oem", .size = 3_GiB}}}))
      << "Should not be able to grow over size of super / 2";
}

TEST_P(BootControlAndroidGroupTestP, AddPartitionToGroup) {
  ExpectStoreMetadata(PartitionMetadata{
      .groups = {
          {.name = T("android"),
           .size = 3_GiB,
           .partitions = {{.name = T("system"), .size = 2_GiB},
                          {.name = T("product_services"), .size = 1_GiB}}}}});
  ExpectRemap({T("system"), T("vendor"), T("product_services")});

  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {
              {.name = "android",
               .size = 3_GiB,
               .partitions = {{.name = "system", .size = 2_GiB},
                              {.name = "product_services", .size = 1_GiB}}},
              SimpleGroup("oem", 2_GiB, "vendor", 2_GiB)}}));
  ExpectDevicesAreMapped({T("system"), T("vendor"), T("product_services")});
}

TEST_P(BootControlAndroidGroupTestP, RemovePartitionFromGroup) {
  ExpectStoreMetadata(PartitionMetadata{
      .groups = {{.name = T("android"), .size = 3_GiB, .partitions = {}}}});
  ExpectRemap({T("vendor")});

  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {{.name = "android", .size = 3_GiB, .partitions = {}},
                     SimpleGroup("oem", 2_GiB, "vendor", 2_GiB)}}));
  ExpectDevicesAreMapped({T("vendor")});
}

TEST_P(BootControlAndroidGroupTestP, AddGroup) {
  ExpectStoreMetadata(PartitionMetadata{
      .groups = {
          SimpleGroup(T("new_group"), 2_GiB, T("new_partition"), 2_GiB)}});
  ExpectRemap({T("system"), T("vendor"), T("new_partition")});

  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {
              SimpleGroup("android", 2_GiB, "system", 2_GiB),
              SimpleGroup("oem", 1_GiB, "vendor", 1_GiB),
              SimpleGroup("new_group", 2_GiB, "new_partition", 2_GiB)}}));
  ExpectDevicesAreMapped({T("system"), T("vendor"), T("new_partition")});
}

TEST_P(BootControlAndroidGroupTestP, RemoveGroup) {
  ExpectStoreMetadataMatch(Not(HasGroup(T("oem"))));
  ExpectRemap({T("system")});
  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {SimpleGroup("android", 2_GiB, "system", 2_GiB)}}));
  ExpectDevicesAreMapped({T("system")});
}

TEST_P(BootControlAndroidGroupTestP, ResizeGroup) {
  ExpectStoreMetadata(PartitionMetadata{
      .groups = {SimpleGroup(T("android"), 2_GiB, T("system"), 2_GiB),
                 SimpleGroup(T("oem"), 3_GiB, T("vendor"), 3_GiB)}});
  ExpectRemap({T("system"), T("vendor")});

  EXPECT_TRUE(bootctl_.InitPartitionMetadata(
      target(),
      PartitionMetadata{
          .groups = {SimpleGroup("android", 2_GiB, "system", 2_GiB),
                     SimpleGroup("oem", 3_GiB, "vendor", 3_GiB)}}));
  ExpectDevicesAreMapped({T("system"), T("vendor")});
}

INSTANTIATE_TEST_CASE_P(BootControlAndroidTest,
                        BootControlAndroidGroupTestP,
                        testing::Values(TestParam{0, 1}, TestParam{1, 0}));

}  // namespace chromeos_update_engine
