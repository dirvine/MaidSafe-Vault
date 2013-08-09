/* Copyright 2012 MaidSafe.net limited

This MaidSafe Software is licensed under the MaidSafe.net Commercial License, version 1.0 or later,
and The General Public License (GPL), version 3. By contributing code to this project You agree to
the terms laid out in the MaidSafe Contributor Agreement, version 1.0, found in the root directory
of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also available at:

http://www.novinet.com/license

Unless required by applicable law or agreed to in writing, software distributed under the License is
distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied. See the License for the specific language governing permissions and limitations under the
License.
*/

#ifndef MAIDSAFE_VAULT_DEMULTIPLEXER_H_
#define MAIDSAFE_VAULT_DEMULTIPLEXER_H_

#include <string>

#include "maidsafe/common/types.h"
#include "maidsafe/routing/routing_api.h"
#include "maidsafe/nfs/message.h"
#include "maidsafe/vault/service.h"

namespace maidsafe {

namespace vault {

class MaidManagerService;
class VersionManagerService;
class DataManagerService;
class PmidManagerService;
class PmidNodeService;

class Demultiplexer {
 public:
  Demultiplexer(MaidManagerService& maid_manager_service,
                VersionManagerService& version_manager_service,
                DataManagerService& data_manager_service,
                PmidManagerService& pmid_manager_service,
                PmidNodeService& pmid_node_service);
//  bool GetFromCache(std::string& serialised_message);
//  void StoreInCache(const std::string& serialised_message);
  template <typename T>
  void HandleMessage(const T& routing_message);

 private:
  template<typename MessageType>
//  NonEmptyString HandleGetFromCache(const nfs::Message& message);
//  void HandleStoreInCache(const nfs::Message& message);

  Service<MaidManagerService>& maid_manager_service_;
  Service<VersionManagerService>& version_manager_service_;
  Service<DataManagerService>& data_manager_service_;
  Service<PmidManagerService>& pmid_manager_service_;
  Service<PmidNodeService>& pmid_node_service_;
};

template <typename T>
void Demultiplexer::HandleMessage(const T& routing_message) {
  auto wrapper_tuple(ParseMessageWrapper(routing_message.contents));
  switch (std::get<1>(wrapper_tuple)) {
    case nfs::Persona::kMaidManager:
      return maid_manager_service_.HandleMessage(wrapper_tuple, routing_message.sender,
                                                 routing_message.receiver);
    case nfs::Persona::kDataManager:
      return data_manager_service_.HandleMessage(wrapper_tuple, routing_message.sender,
                                                 routing_message.receiver);
    case nfs::Persona::kPmidManager:
      return pmid_manager_service_.HandleMessage(wrapper_tuple, routing_message.sender,
                                                 routing_message.receiver);
    case nfs::Persona::kPmidNode:
      return pmid_node_.HandleMessage(wrapper_tuple, routing_message.sender,
                                      routing_message.receiver);
    default:
      LOG(kError) << "Unhandled Persona";
  }
}

}  // namespace vault

}  // namespace maidsafe

#endif  // MAIDSAFE_VAULT_DEMULTIPLEXER_H_
