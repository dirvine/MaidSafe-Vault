/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */



#include <algorithm>

#include "maidsafe/common/test.h"
#include "maidsafe/vault/tests/tests_utils.h"
#include "maidsafe/vault/tests/vault_network.h"

namespace maidsafe {

namespace vault {

namespace test {

typedef StructuredDataVersions::VersionName VersionName;

class VersionHandlerTest : public VaultNetwork  {
 public:
  VersionHandlerTest() {}

  template <typename DataName>
  std::vector<VersionName> GetVersions(const DataName& data_name) {
    try {
      auto future(clients_.front()->nfs_->GetVersions(data_name));
      return future.get();
    } catch(const maidsafe_error&) {
      throw;
    }
  }
};

TEST_F(VersionHandlerTest, FUNC_PutGet) {
  EXPECT_TRUE(AddClient(true));
  ImmutableData chunk(NonEmptyString(RandomAlphaNumericString(1024)));
  StructuredDataVersions::VersionName v_aaa(0, ImmutableData::Name(Identity(std::string(64, 'a'))));
  EXPECT_NO_THROW(clients_.front()->nfs_->PutVersion(
                      chunk.name(), StructuredDataVersions::VersionName(), v_aaa));
  Sleep(std::chrono::seconds(2));
  try {
    auto future(clients_.front()->nfs_->GetVersions(chunk.name()));
    auto versions(future.get());
    EXPECT_EQ(versions.front().id, v_aaa.id);
  } catch(const maidsafe_error& error) {
    EXPECT_TRUE(false) << "Failed to retrieve version: " << error.what();
  }
}

TEST_F(VersionHandlerTest, FUNC_DeleteBranchUntilFork) {
  EXPECT_TRUE(AddClient(true));
  ImmutableData::Name name(Identity(RandomAlphaNumericString(64)));
  VersionName v0_aaa(0, ImmutableData::Name(Identity(std::string(64, 'a'))));
  VersionName v1_bbb(1, ImmutableData::Name(Identity(std::string(64, 'b'))));
  VersionName v2_ccc(2, ImmutableData::Name(Identity(std::string(64, 'c'))));
  VersionName v2_ddd(2, ImmutableData::Name(Identity(std::string(64, 'd'))));
  VersionName v3_fff(3, ImmutableData::Name(Identity(std::string(64, 'f'))));
  VersionName v4_iii(4, ImmutableData::Name(Identity(std::string(64, 'i'))));

  std::vector<std::pair<VersionName, VersionName>> puts;
  puts.push_back(std::make_pair(v0_aaa, v1_bbb));
  puts.push_back(std::make_pair(v1_bbb, v2_ccc));
  puts.push_back(std::make_pair(v2_ccc, v3_fff));
  puts.push_back(std::make_pair(v1_bbb, v2_ddd));
  puts.push_back(std::make_pair(v3_fff, v4_iii));

  EXPECT_NO_THROW(clients_.front()->nfs_->PutVersion(name, VersionName(), v0_aaa));
  Sleep(std::chrono::seconds(2));

  for (const auto& put : puts) {
    EXPECT_NO_THROW(clients_.front()->nfs_->PutVersion(name, put.first, put.second));
    Sleep(std::chrono::seconds(2));
  }

  try {
    auto versions(GetVersions(name));
    for (const auto& version : versions)
      LOG(kVerbose) << version.id->string();
    EXPECT_NE(std::find(std::begin(versions), std::end(versions), v4_iii), std::end(versions));
    EXPECT_NE(std::find(std::begin(versions), std::end(versions), v2_ddd), std::end(versions));
  }
  catch (const std::exception& error) {
    EXPECT_TRUE(false) << "Version should have existed " << error.what();
  }

  EXPECT_NO_THROW(clients_.front()->nfs_->DeleteBranchUntilFork(name, v4_iii));
  Sleep(std::chrono::seconds(4));
  LOG(kVerbose) << "After delete";

  try {
    auto versions(GetVersions(name));
    LOG(kVerbose) << "versions.size: " << versions.size();
    for (const auto& version : versions)
      LOG(kVerbose) << version.id->string();
    EXPECT_EQ(std::find(std::begin(versions), std::end(versions), v4_iii), std::end(versions));
    EXPECT_NE(std::find(std::begin(versions), std::end(versions), v2_ddd), std::end(versions));
  }
  catch (const std::exception& error) {
    EXPECT_TRUE(false) << error.what();
  }
}

}  // namespace test

}  // namespace vault

}  // namespace maidsafe
