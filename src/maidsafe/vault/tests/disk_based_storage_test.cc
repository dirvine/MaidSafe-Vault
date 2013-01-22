/*******************************************************************************
*  Copyright 2012 maidsafe.net limited                                        *
*                                                                             *
*  The following source code is property of maidsafe.net limited and is not   *
*  meant for external use.  The use of this code is governed by the licence   *
*  file licence.txt found in the root of this directory and also on           *
*  www.maidsafe.net.                                                          *
*                                                                             *
*  You are not free to copy, amend or otherwise use this source code without  *
*  the explicit written permission of the board of directors of maidsafe.net. *
******************************************************************************/

#include "maidsafe/vault/disk_based_storage.h"

#include <memory>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"

#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/common/test.h"

#include "maidsafe/passport/types.h"

#include "maidsafe/nfs/message.h"

#include "maidsafe/vault/utils.h"


namespace fs = boost::filesystem;

namespace maidsafe {

namespace vault {

namespace test {

typedef std::map<std::string, std::pair<uint32_t, std::string>> ElementMap;

template <typename T>
class DiskStorageTest : public testing::Test {
 public:
  DiskStorageTest()
      : root_directory_(maidsafe::test::CreateTestPath("MaidSafe_Test_DiskStorage")) {}

 protected:
  maidsafe::test::TestPath root_directory_;

  std::string GenerateFileContent(uint32_t max_file_size) {
    protobuf::DiskStoredFile disk_file;
    protobuf::DiskStoredElement disk_element;
    disk_element.set_data_name(RandomString(64));
    disk_element.set_version(RandomUint32() % 100);
    disk_element.set_serialised_value(RandomString(RandomUint32() % max_file_size));
    disk_file.add_disk_element()->CopyFrom(disk_element);
    return disk_file.SerializeAsString();
  }

  void ChangeDiskElement(std::string& serialised_disk_element,
                         const std::string& new_serialised_value) {
    protobuf::DiskStoredElement disk_element;
    assert(disk_element.ParseFromString(serialised_disk_element) &&
           "Received element doesn't parse.");
    disk_element.set_serialised_value(new_serialised_value);
    serialised_disk_element = disk_element.SerializeAsString();
  }

  bool VerifyElements(const DiskBasedStorage& disk_based_storage,
                      const ElementMap& element_list) {
    std::future<DiskBasedStorage::PathVector> result_get_file_paths =
        disk_based_storage.GetFileNames();
    DiskBasedStorage::PathVector file_paths(result_get_file_paths.get());
    EXPECT_EQ(file_paths.size(), 1U);

    auto result = disk_based_storage.GetFile(file_paths[0]);
    NonEmptyString fetched_content = result.get();
    protobuf::DiskStoredFile fetched_disk_file;
    EXPECT_TRUE(fetched_disk_file.ParseFromString(fetched_content.string()));
    EXPECT_EQ(fetched_disk_file.disk_element_size(), element_list.size());
    for (auto itr(element_list.begin()); itr != element_list.end(); ++itr) {
      protobuf::DiskStoredElement element;
      element.ParseFromString((*itr).second.second);
      bool found_match(false);
      for (int i(0); i < fetched_disk_file.disk_element_size(); ++i)
        if (detail::MatchingDiskElements(fetched_disk_file.disk_element(i), element))
          found_match = true;
      EXPECT_TRUE(found_match) << "can't find match element for element " << (*itr).second.first;
      if (!found_match)
        return false;
    }
    return true;
  }
};

typedef testing::Types<passport::PublicAnmid,
                       passport::PublicAnsmid,
                       passport::PublicAntmid,
                       passport::PublicAnmaid,
                       passport::PublicMaid,
                       passport::PublicPmid,
                       passport::Mid,
                       passport::Smid,
                       passport::Tmid,
                       passport::PublicAnmpid,
                       passport::PublicMpid> AllTypes;

TYPED_TEST_CASE(DiskStorageTest, AllTypes);

TYPED_TEST(DiskStorageTest, BEH_ConstructorDestructor) {
  fs::path root_path(*(this->root_directory_) / RandomString(6));
  boost::system::error_code error_code;
  EXPECT_FALSE(fs::exists(root_path, error_code));
  {
    DiskBasedStorage disk_based_storage(root_path);
    EXPECT_TRUE(fs::exists(root_path, error_code));
    // An empty file shall be generated in constructor
    std::future<uint32_t> file_count(disk_based_storage.GetFileCount());
    EXPECT_EQ(file_count.get(), 1);
  }
  EXPECT_TRUE(fs::exists(root_path, error_code));
}

TYPED_TEST(DiskStorageTest, BEH_FileHandlers) {
  fs::path root_path(*(this->root_directory_) / RandomString(6));
  DiskBasedStorage disk_based_storage(root_path);
  std::map<fs::path, NonEmptyString> files;
  uint32_t num_files(100), max_file_size(10000);
  std::vector<uint32_t> file_numbers;
  for (uint32_t i(0); i < num_files; ++i)
    file_numbers.push_back(i);
  std::random_shuffle(file_numbers.begin(), file_numbers.end());

  for (uint32_t i(0); i < num_files; ++i) {
    NonEmptyString file_content(this->GenerateFileContent(max_file_size));
    std::string hash(EncodeToBase32(crypto::Hash<crypto::SHA512>(file_content)));
    std::string file_name(std::to_string(file_numbers[i]) + "." + hash);
    fs::path file_path(root_path / file_name);
    files.insert(std::make_pair(file_path, file_content));
    disk_based_storage.PutFile(file_path, file_content);
  }

  std::future<uint32_t> file_count(disk_based_storage.GetFileCount());
  EXPECT_EQ(file_count.get(), num_files);
  std::future<DiskBasedStorage::PathVector> file_paths = disk_based_storage.GetFileNames();
  EXPECT_EQ(file_paths.get().size(), num_files);

  boost::system::error_code error_code;
  auto itr = files.begin();
  do {
    fs::path path((*itr).first);
    EXPECT_TRUE(fs::exists(path, error_code));

    auto result = disk_based_storage.GetFile(path);
    NonEmptyString content(result.get());
    EXPECT_EQ(content, (*itr).second);

    ++itr;
  } while (itr != files.end());
}

TYPED_TEST(DiskStorageTest, BEH_FileHandlersWithCorruptingThread) {
  // File handlers of DiskBasedStorage are non-blocking, using a separate Active object
  Active active;
  fs::path root_path(*(this->root_directory_) / RandomString(6));
  DiskBasedStorage disk_based_storage(root_path);
  std::map<fs::path, NonEmptyString> files;
  uint32_t num_files(10), max_file_size(10000);
  for (uint32_t i(0); i < num_files; ++i) {
    NonEmptyString file_content(this->GenerateFileContent(max_file_size));
    std::string hash(EncodeToBase32(crypto::Hash<crypto::SHA512>(file_content)));
    std::string file_name(std::to_string(i) + "." + hash);
    fs::path file_path(root_path / file_name);
    files.insert(std::make_pair(file_path, file_content));
  }

  for (auto itr(files.begin()); itr != files.end(); ++itr) {
    fs::path file_path((*itr).first);
    active.Send([file_path] () {
                  maidsafe::WriteFile(file_path, RandomString(100));
                });
    disk_based_storage.PutFile(file_path, (*itr).second);
  }

  std::future<uint32_t> file_count(disk_based_storage.GetFileCount());
  EXPECT_EQ(file_count.get(), num_files);

//   Active active_delete;
  boost::system::error_code error_code;
  auto itr = files.begin();
  do {
    fs::path path((*itr).first);
    EXPECT_TRUE(fs::exists(path, error_code));

    active.Send([&disk_based_storage, path] () {
                  auto result = disk_based_storage.GetFile(path);
                  do {
                    Sleep(boost::posix_time::milliseconds(1));
                  } while (!result.valid());
                  EXPECT_FALSE(result.has_exception()) << "Get exception when trying to get "
                                                       << path.filename();
                  if (!result.has_exception()) {
                    NonEmptyString content(result.get());
                    EXPECT_TRUE(content.IsInitialised());
                  }
                });
//     active_delete.Send([path] () { fs::remove(path); });

    ++itr;
  } while (itr != files.end());
}

TYPED_TEST(DiskStorageTest, BEH_ElementHandlers) {
  fs::path root_path(*(this->root_directory_) / RandomString(6));
  DiskBasedStorage disk_based_storage(root_path);

  typename TypeParam::name_type name((Identity(RandomString(crypto::SHA512::DIGESTSIZE))));
  int32_t version(RandomUint32());
  std::string serialised_value(RandomString(10000));
  disk_based_storage.Store<TypeParam>(name, version, serialised_value);

  protobuf::DiskStoredElement element;
  element.set_data_name(name.data.string());
  element.set_version(version);
  element.set_serialised_value(serialised_value);
  protobuf::DiskStoredFile disk_file;
  disk_file.add_disk_element()->CopyFrom(element);
  std::string hash = EncodeToBase32(crypto::Hash<crypto::SHA512>(disk_file.SerializeAsString()));
  fs::path file_path = detail::GetFilePath(
      root_path, hash, disk_based_storage.GetFileCount().get() - 1);

  Sleep(boost::posix_time::milliseconds(10));
  boost::system::error_code error_code;
  EXPECT_TRUE(fs::exists(file_path, error_code));
  {
    auto result = disk_based_storage.GetFile(file_path);
    NonEmptyString fetched_content = result.get();
    EXPECT_EQ(fetched_content.string(), disk_file.SerializeAsString());
  }

  std::string new_serialised_value(RandomString(10000));
  element.set_serialised_value(new_serialised_value);
  disk_file.clear_disk_element();
  disk_file.add_disk_element()->CopyFrom(element);

  disk_based_storage.Modify<TypeParam>(name, version,
                                       [this, new_serialised_value]
                                          (std::string& serialised_disk_element) {
                                         this->ChangeDiskElement(serialised_disk_element,
                                                                 new_serialised_value); },
                                       serialised_value);

  std::string new_hash = EncodeToBase32(
                            crypto::Hash<crypto::SHA512>(disk_file.SerializeAsString()));
  fs::path new_file_path = detail::GetFilePath(
      root_path, new_hash, disk_based_storage.GetFileCount().get() - 1);
  {
    auto result = disk_based_storage.GetFile(new_file_path);
    NonEmptyString fetched_content = result.get();
    EXPECT_EQ(fetched_content.string(), disk_file.SerializeAsString());
    EXPECT_FALSE(fs::exists(file_path, error_code));
    EXPECT_TRUE(fs::exists(new_file_path, error_code));
  }

  disk_based_storage.Delete<TypeParam>(name, version);
  Sleep(boost::posix_time::milliseconds(10));
  EXPECT_FALSE(fs::exists(new_file_path, error_code));
}

TYPED_TEST(DiskStorageTest, BEH_ElementHandlersWithMultThreads) {
  fs::path root_path(*(this->root_directory_) / EncodeToBase32(RandomString(6)));
  DiskBasedStorage disk_based_storage(root_path);
  std::vector<std::shared_ptr<Active>> active_list;
  ElementMap element_list;
  uint32_t num_files(10), max_file_size(10000);

  for (uint32_t i(0); i < num_files; ++i) {
    std::shared_ptr<Active> active_ptr;
    active_ptr.reset(new Active());
    active_list.push_back(active_ptr);

    typename TypeParam::name_type name((Identity(RandomString(crypto::SHA512::DIGESTSIZE))));
    int32_t version(RandomUint32());
    std::string serialised_value(EncodeToBase32(RandomString(max_file_size)));

    protobuf::DiskStoredElement element;
    element.set_data_name(name.data.string());
    element.set_version(version);
    element.set_serialised_value(serialised_value);

    element_list.insert(std::make_pair(serialised_value,
                                       std::make_pair(i, element.SerializeAsString())));
    disk_based_storage.Store<TypeParam>(name, version, serialised_value);
  }

  EXPECT_TRUE(this->VerifyElements(disk_based_storage, element_list));

  // Generate new content for each element
  for (auto itr(element_list.begin()); itr != element_list.end(); ++itr) {
    std::string new_serialised_value(EncodeToBase32(RandomString(max_file_size)));
    protobuf::DiskStoredElement element;
    element.ParseFromString((*itr).second.second);
    element.set_serialised_value(new_serialised_value);
    (*itr).second = std::make_pair((*itr).second.first, element.SerializeAsString());
  }

  // Modify each element's content parallel
  auto itr = element_list.begin();
  int i = 0;
  do {
    protobuf::DiskStoredElement element;
    element.ParseFromString((*itr).second.second);
    typename TypeParam::name_type name((Identity(element.data_name())));
    int32_t version = element.version();
    std::string old_serialised_value = (*itr).first;
    std::string new_serialised_value = element.serialised_value();

    active_list[i]->Send([this, &disk_based_storage,
                          name, version, old_serialised_value, new_serialised_value] () {
                            disk_based_storage.Modify<TypeParam>(name, version,
                                [this, new_serialised_value]
                                    (std::string& serialised_disk_element) {
                                      this->ChangeDiskElement(serialised_disk_element,
                                                              new_serialised_value); },
                                old_serialised_value);
                          });
    ++itr;
    ++i;
  } while (itr != element_list.end());

  Sleep(boost::posix_time::milliseconds(100));

  EXPECT_TRUE(this->VerifyElements(disk_based_storage, element_list));

  // Parallel delete all elements
  itr = element_list.begin();
  i = 0;
  do {
    protobuf::DiskStoredElement element;
    element.ParseFromString((*itr).second.second);
    typename TypeParam::name_type name((Identity(element.data_name())));
    int32_t version = element.version();
    active_list[i]->Send([&disk_based_storage, name, version] () {
                            disk_based_storage.Delete<TypeParam>(name, version);
                          });
    ++itr;
    ++i;
  } while (itr != element_list.end());

  Sleep(boost::posix_time::milliseconds(100));

  {
    std::future<DiskBasedStorage::PathVector> result_get_file_paths =
        disk_based_storage.GetFileNames();
    DiskBasedStorage::PathVector file_paths(result_get_file_paths.get());
    EXPECT_EQ(file_paths.size(), 0U);
  }
}

}  // namespace test

}  // namespace vault

}  // namespace maidsafe

