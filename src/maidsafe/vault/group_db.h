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

#ifndef MAIDSAFE_VAULT_GROUP_DB_H_
#define MAIDSAFE_VAULT_GROUP_DB_H_

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "leveldb/db.h"

#include "maidsafe/common/error.h"
#include "maidsafe/common/on_scope_exit.h"
#include "maidsafe/common/types.h"
#include "maidsafe/common/visualiser_log.h"
#include "maidsafe/vault/utils.h"
#include "maidsafe/vault/config.h"
#include "maidsafe/vault/pmid_manager/pmid_manager.h"

namespace maidsafe {

namespace vault {

// All public methods provide strong exception guarantee
template <typename Persona>
class GroupDb {
 public:
  typedef typename Persona::GroupName GroupName;
  typedef typename Persona::Key Key;
  typedef typename Persona::Value Value;
  typedef typename Persona::Metadata Metadata;
  typedef std::pair<Key, Value> KvPair;
  struct Contents;
  typedef std::map<NodeId, std::vector<Contents>> TransferInfo;

  struct Contents {
    Contents() : group_name(), metadata(), kv_pairs() {}

    Contents(Contents&& other)
        : group_name(std::move(other.group_name)),
          metadata(std::move(other.metadata)),
          kv_pairs(std::move(other.kv_pairs))  {}

    GroupName group_name;
    Metadata metadata;
    std::vector<KvPair> kv_pairs;
   private:
    Contents(const Contents& other);
  };

  explicit GroupDb(const boost::filesystem::path& db_path);
  ~GroupDb();

  void AddGroup(const GroupName& group_name, const Metadata& metadata);
  // use only in case of leaving or unregister
  void DeleteGroup(const GroupName& group_name);
  // For atomically updating metadata only
  void Commit(const GroupName& group_name, std::function<void(Metadata& metadata)> functor);
  // For atomically updating metadata and value
  std::unique_ptr<Value> Commit(const Key& key,
      std::function<detail::DbAction(Metadata& metadata, std::unique_ptr<Value>& value)> functor);
  TransferInfo GetTransferInfo(std::shared_ptr<routing::MatrixChange> matrix_change);
  void HandleTransfer(const Contents& content);

  // returns metadata if group_name exists in db
  Metadata GetMetadata(const GroupName& group_name);
  // returns value if key exists in db
  Value GetValue(const Key& key);
  Contents GetContents(const GroupName& group_name);

 private:
  typedef uint32_t GroupId;
  typedef std::map<GroupName, std::pair<GroupId, Metadata>> GroupMap;
  GroupDb(const GroupDb&);
  GroupDb& operator=(const GroupDb&);
  GroupDb(GroupDb&&);
  GroupDb& operator=(GroupDb&&);

  typename GroupMap::iterator AddGroupToMap(const GroupName& group_name, const Metadata& metadata);
  void UpdateGroup(typename GroupMap::iterator itr);

  void DeleteGroupEntries(const GroupName& group_name);
  void DeleteGroupEntries(typename GroupMap::iterator itr);
  Contents GetContents(typename GroupMap::iterator it);
  void ApplyTransfer(const Contents& /*contents*/);
  Value Get(const Key& key, const GroupId& group_id);
  void Put(const KvPair& key_value_pair, const GroupId& group_id);
  void Delete(const Key& key, const GroupId& group_id);
  std::string MakeLevelDbKey(const GroupId& group_id, const Key& key);
  Key MakeKey(const GroupName group_name, const leveldb::Slice& level_db_key);
  uint32_t GetGroupId(const leveldb::Slice& level_db_key) const;
  typename GroupMap::iterator FindGroup(const GroupName& group_name);
  typename GroupMap::iterator FindOrCreateGroup(const GroupName& group_name);

  static const int kPrefixWidth_ = 2;
  const boost::filesystem::path kDbPath_;
  std::mutex mutex_;
  std::unique_ptr<leveldb::DB> leveldb_;
  GroupMap group_map_;
};

template <>
GroupDb<PmidManager>::GroupMap::iterator GroupDb<PmidManager>::FindOrCreateGroup(
    const GroupName& group_name);

template <>
void GroupDb<PmidManager>::UpdateGroup(typename GroupMap::iterator itr);

template <typename Persona>
GroupDb<Persona>::GroupDb(const boost::filesystem::path& db_path)
    : kDbPath_(db_path),
      mutex_(),
      leveldb_(InitialiseLevelDb(kDbPath_)),
      group_map_() {
#if defined(__GNUC__) && (!defined(MAIDSAFE_APPLE) && !(defined(_MSC_VER) && _MSC_VER == 1700))
  // Remove this assert if value needs to be copy constructible.
  // this is just a check to avoid copy constructor unless we require it
  static_assert(!std::is_copy_constructible<typename Persona::Value>::value,
                "value should not be copy constructible !");
#endif

#ifndef _MSC_VER
  static_assert(std::is_move_constructible<typename Persona::Value>::value,
                "value should be move constructible !");
#endif
}

template <typename Persona>
GroupDb<Persona>::~GroupDb() {
  try {
    leveldb::DestroyDB(kDbPath_.string(), leveldb::Options());
    boost::filesystem::remove_all(kDbPath_);
  } catch (const std::exception& e) {
    LOG (kError) << "Failed to remove db : " << boost::diagnostic_information(e);
  }
}

template <typename Persona>
void GroupDb<Persona>::AddGroup(const GroupName& group_name, const Metadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  AddGroupToMap(group_name, metadata);
}

template <typename Persona>
typename GroupDb<Persona>::GroupMap::iterator GroupDb<Persona>::AddGroupToMap(
    const GroupName& group_name, const Metadata& metadata) {
  static const uint64_t kGroupsLimit(static_cast<GroupId>(std::pow(256, kPrefixWidth_)));
  if (group_map_.size() == kGroupsLimit - 1)
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
  GroupId group_id(RandomInt32() % kGroupsLimit);
  while (std::any_of(std::begin(group_map_), std::end(group_map_),
                     [&group_id](const std::pair<GroupName, std::pair<GroupId, Metadata>>&
                                 element) { return group_id == element.second.first; })) {
    group_id = RandomInt32() % kGroupsLimit;
  }
  LOG(kVerbose) << "GroupDb<Persona>::AddGroupToMap size of group_map_ " << group_map_.size()
                << " current group_name " << HexSubstr(group_name->string());
  auto ret_val = group_map_.insert(std::make_pair(group_name, std::make_pair(group_id, metadata)));
  if (!ret_val.second) {
    LOG(kError) << "account already exists in the group map";
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::account_already_exists));
  }
  LOG(kInfo) << "group inserting succeeded for group_name "
             << HexSubstr(group_name->string());
  return ret_val.first;
}

template <typename Persona>
void GroupDb<Persona>::DeleteGroup(const GroupName& group_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  DeleteGroupEntries(group_name);
}

template <typename Persona>
void GroupDb<Persona>::Commit(const GroupName& group_name,
                              std::function<void(Metadata& metadata)> functor) {
  LOG(kVerbose) << "GroupDb<Persona>::Commit update metadata for account "
                << HexSubstr(group_name->string());
  assert(functor);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it(FindOrCreateGroup(group_name));
  on_scope_exit update_group([it, this]() { UpdateGroup(it); });
  functor(it->second.second);
}

template <typename Persona>
std::unique_ptr<typename Persona::Value> GroupDb<Persona>::Commit(
    const Key& key,
    std::function<detail::DbAction(Metadata& metadata, std::unique_ptr<Value>& value)> functor) {
  LOG(kVerbose) << "GroupDb<Persona>::Commit update metadata and value for account "
                << HexSubstr(key.group_name()->string());
  assert(functor);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it(FindOrCreateGroup(key.group_name()));
  on_scope_exit update_group([it, this]() { UpdateGroup(it); });
  std::unique_ptr<Value> value;
  try {
    value.reset(new Value(Get(key, it->second.first)));
  } catch (const maidsafe_error& error) {
    LOG(kError) << "GroupDb<Persona>::Commit encountered error "
                << boost::diagnostic_information(error);
    if (error.code() != make_error_code(CommonErrors::no_such_element))
      throw error;  // throw only for db errors
  }

  try {
    if (detail::DbAction::kPut == functor(it->second.second, value)) {
      LOG(kInfo) << "detail::DbAction::kPut";
      assert(value);
      if (!value)
        BOOST_THROW_EXCEPTION(MakeError(CommonErrors::null_pointer));
      Put(std::make_pair(key, std::move(*value)), it->second.first);
    } else {
      LOG(kInfo) << "detail::DbAction::kDelete";
      if (value) {
        Delete(key, it->second.first);
        return value;
      } else {
        LOG(kError) << "value is not initialised";
      }
    }
  } catch (const maidsafe_error& error) {
    LOG(kError) << "GroupDb<Persona>::Commit encountered error "
                << boost::diagnostic_information(error);
    throw error;
  }
  return nullptr;
}

template <typename Persona>
typename GroupDb<Persona>::Contents GroupDb<Persona>::GetContents(const GroupName& group_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it(FindGroup(group_name));
  return GetContents(it);
}

template <typename Persona>
typename GroupDb<Persona>::Contents GroupDb<Persona>::GetContents(typename GroupMap::iterator it) {
  Contents contents;
  contents.group_name = it->first;
  contents.metadata = it->second.second;
  // get db entry
  std::unique_ptr<leveldb::Iterator> iter(leveldb_->NewIterator(leveldb::ReadOptions()));
  const auto group_id = it->second.first;
  const auto group_id_str = detail::ToFixedWidthString<kPrefixWidth_>(group_id);
  for (iter->Seek(group_id_str); (iter->Valid() && (GetGroupId(iter->key()) == group_id));
       iter->Next()) {
    contents.kv_pairs.push_back(std::make_pair(MakeKey(contents.group_name, iter->key()),
                                               Value(iter->value().ToString())));
  }
  iter.reset();
  return contents;
}

template <typename Persona>
typename GroupDb<Persona>::TransferInfo GroupDb<Persona>::GetTransferInfo(
    std::shared_ptr<routing::MatrixChange> matrix_change) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<GroupName> prune_vector;
  TransferInfo transfer_info;
  LOG(kVerbose) << "GroupDb<Persona>::GetTransferInfo group_map_.size() " << group_map_.size();
  for (auto group_itr(group_map_.begin()); group_itr != group_map_.end(); ++group_itr) {
    auto check_holder_result = matrix_change->CheckHolders(NodeId(group_itr->first->string()));
    if (check_holder_result.proximity_status == routing::GroupRangeStatus::kInRange) {
      LOG(kVerbose) << "GroupDb<Persona>::GetTransferInfo in range ";
      if (check_holder_result.new_holders.size() != 0) {
        LOG(kVerbose) << "GroupDb<Persona>::GetTransferInfo having new node "
                      << DebugId(check_holder_result.new_holders.at(0));
//         assert(check_holder_result.new_holders.size() == 1);
        if (check_holder_result.new_holders.size() != 1)
          LOG(kError) << "having " << check_holder_result.new_holders.size()
                      << " new holders, only the first one got processed";
        auto found_itr = transfer_info.find(check_holder_result.new_holders.at(0));
        if (found_itr != transfer_info.end()) {  // Add to map
          found_itr->second.push_back(GetContents(group_itr));
        } else {  // create contents add to map
          LOG(kVerbose) << "GroupDb<Persona>::GetTransferInfo transfering account "
                        << HexSubstr(group_itr->first->string()) << " to "
                        << DebugId(check_holder_result.new_holders.at(0));
          std::vector<Contents>  contents_vector;
          contents_vector.push_back(std::move(GetContents(group_itr)));
          transfer_info[check_holder_result.new_holders.at(0)] = std::move(contents_vector);
        }
      }
    } else {  // Prune group
      VLOG(VisualiserAction::kRemoveAccount, Identity{ group_itr->first->string() });
      prune_vector.push_back(group_itr->first);
    }
  }
  LOG(kVerbose) << "GroupDb<Persona>::GetTransferInfo prune_vector.size() " << prune_vector.size();
  for (const auto& i : prune_vector)
    DeleteGroupEntries(i);
  return transfer_info;
}

// FIXME (Prakash)
template <typename Persona>
void GroupDb<Persona>::HandleTransfer(const Contents& content) {
  std::lock_guard<std::mutex> lock(mutex_);
  ApplyTransfer(content);
}

// Ignores values which are already in db ?
// Need discussion related to pmid account creation case. Pmid account will be created on
// put action. This means a valid account transfer will be ignored.
template <typename Persona>
void GroupDb<Persona>::ApplyTransfer(const Contents& contents) {
  // BEFORE_RELEASE what if metadata can't got resolved ? i.e. metadata is empty
  //                create an empty account only for group_name?
  typename GroupMap::iterator itr;
  try {
    itr = FindGroup(contents.group_name);
  } catch(...) {
    // During the transfer, there is chance one account's actions scattered across different vaults
    // this will incur multiple AddGroupToMap attempts for the same account
    LOG(kWarning) << "trying to transfer part of an already existed account";
    itr = AddGroupToMap(contents.group_name, contents.metadata);
  }
  for (const auto& kv_pair : contents.kv_pairs) {
    try {
      Put(kv_pair, itr->second.first);
    } catch(...) {
      LOG(kError) << "trying to re-insert an existing entry";
    }
  }
}

template <typename Persona>
typename GroupDb<Persona>::Metadata GroupDb<Persona>::GetMetadata(const GroupName& group_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it(FindGroup(group_name));
  return it->second.second;
}


template <typename Persona>
typename GroupDb<Persona>::Value GroupDb<Persona>::GetValue(const Key& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it(FindGroup(key.group_name()));
  return Get(key, it->second.first);
}

template <typename Persona>
void GroupDb<Persona>::DeleteGroupEntries(const GroupName& group_name) {
  try {
    DeleteGroupEntries(FindGroup(group_name));
  } catch (const maidsafe_error& error) {
    LOG(kInfo) << "account doesn't exist for group "
               << HexSubstr(group_name->string()) << ", error : "
               << boost::diagnostic_information(error);
  }
}

template <typename Persona>
void GroupDb<Persona>::DeleteGroupEntries(typename GroupMap::iterator it) {
  assert(it != group_map_.end());
  std::vector<std::string> group_db_keys;
  const auto group_id = it->second.first;
  const auto group_id_str = detail::ToFixedWidthString<kPrefixWidth_>(group_id);
  std::unique_ptr<leveldb::Iterator> iter(leveldb_->NewIterator(leveldb::ReadOptions()));
  for (iter->Seek(group_id_str);
       (iter->Valid() && (GetGroupId(iter->key()) == group_id));
       iter->Next())
    group_db_keys.push_back(iter->key().ToString());

  iter.reset();

  for (const auto& key : group_db_keys) {
    leveldb::Status status(leveldb_->Delete(leveldb::WriteOptions(), key));
    if (!status.ok())
      BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
  }
  group_map_.erase(it);
  leveldb_->CompactRange(nullptr, nullptr);
}

// throws
template <typename Persona>
typename GroupDb<Persona>::Value GroupDb<Persona>::Get(const Key& key, const GroupId& group_id) {
  leveldb::ReadOptions read_options;
  read_options.verify_checksums = true;
  std::string value_string;
  leveldb::Status status(
              leveldb_->Get(read_options, MakeLevelDbKey(group_id, key), &value_string));
  if (status.ok()) {
    assert(!value_string.empty());
    return Value(value_string);
  } else if (status.IsNotFound()) {
    LOG(kWarning) << "cann't find such element for get, throwing error";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::no_such_element));
  }
  LOG(kError) << "unknown error";
  BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
}

template <typename Persona>
void GroupDb<Persona>::Put(const KvPair& key_value_pair, const GroupId& group_id) {
  leveldb::Status status(leveldb_->Put(leveldb::WriteOptions(),
                                       MakeLevelDbKey(group_id, key_value_pair.first),
                                       key_value_pair.second.Serialise()));
  if (!status.ok())
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
}

template <typename Persona>
void GroupDb<Persona>::Delete(const Key& key, const GroupId& group_id) {
  leveldb::Status status(
      leveldb_->Delete(leveldb::WriteOptions(), MakeLevelDbKey(group_id, key)));
  if (!status.ok())
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
}

template <typename Persona>
std::string GroupDb<Persona>::MakeLevelDbKey(const GroupId& group_id, const Key& key) {
  return detail::ToFixedWidthString<kPrefixWidth_>(group_id) + key.ToFixedWidthString().string();
}

template <typename Persona>
typename Persona::Key GroupDb<Persona>::MakeKey(const GroupName group_name,
                                                const leveldb::Slice& level_db_key) {
  return Key(group_name, typename Persona::Key::FixedWidthString(
                 (level_db_key.ToString()).substr(kPrefixWidth_)));
}

template <typename Persona>
uint32_t GroupDb<Persona>::GetGroupId(const leveldb::Slice& level_db_key) const {
  return detail::FromFixedWidthString<kPrefixWidth_>(
      (level_db_key.ToString()).substr(0, kPrefixWidth_));
}

// throws
template <typename Persona>
typename GroupDb<Persona>::GroupMap::iterator GroupDb<Persona>::FindGroup(
    const GroupName& group_name) {
  auto it(group_map_.find(group_name));
  if (it == group_map_.end())
    BOOST_THROW_EXCEPTION(MakeError(VaultErrors::no_such_account));
  return it;
}

template <typename Persona>
typename GroupDb<Persona>::GroupMap::iterator GroupDb<Persona>::FindOrCreateGroup(
    const GroupName& group_name) {
  LOG(kInfo) << "GroupDb<Persona>::FindOrCreateGroup generic -- Don't Do Creation --";
  return FindGroup(group_name);
}

template <typename Persona>
void GroupDb<Persona>::UpdateGroup(typename GroupMap::iterator itr) {
  LOG(kInfo) << "GroupDb<Persona>::UpdateGroup updating " << HexSubstr(itr->first->string())
             << ". -- Do Nothing --";
}  // Do Nothing

}  // namespace vault

}  // namespace maidsafe

#endif  // MAIDSAFE_VAULT_GROUP_DB_H_
