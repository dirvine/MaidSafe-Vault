/***************************************************************************************************
 *  Copyright 2012 MaidSafe.net limited                                                            *
 *                                                                                                 *
 *  The following source code is property of MaidSafe.net limited and is not meant for external    *
 *  use.  The use of this code is governed by the licence file licence.txt found in the root of    *
 *  this directory and also on www.maidsafe.net.                                                   *
 *                                                                                                 *
 *  You are not free to copy, amend or otherwise use this source code without the explicit         *
 *  written permission of the board of directors of MaidSafe.net.                                  *
 **************************************************************************************************/

#include "maidsafe/vault/tools/commander.h"

namespace maidsafe {

namespace vault {

namespace tools {

namespace fs = boost::filesystem;
namespace po = boost::program_options;

Commander::Commander(size_t pmids_count, size_t chunk_set_count)
    : pmids_count_(pmids_count),
      chunk_set_count_(chunk_set_count),
      all_pmids_(),
      keys_path_(),
      peer_endpoints_(),
      selected_ops_() {}

void Commander::AnalyseCommandLineOptions(int argc, char* argv[]) {
  po::options_description cmdline_options;
  cmdline_options.add(AddGenericOptions("Commands"))
                 .add(AddConfigurationOptions("Configuration options"));

  po::variables_map variables_map(CheckOptionValidity(cmdline_options, argc, argv));
  GetPathFromProgramOption("keys_path", variables_map);

  // bootstrap endpoint
  if (variables_map.count("peer"))
    peer_endpoints_.push_back(GetBootstrapEndpoint(variables_map.at("peer").as<std::string>()));

  HandleKeys();
  HandleNetWork();
  HandleStore();
  HandleVerify();
  HandleDoTest();
  HandleDeleteKeys();
}

void Commander::GetPathFromProgramOption(const std::string& option_name,
                                         po::variables_map& variables_map) {
  if (variables_map.count(option_name))
    keys_path_ = variables_map.at(option_name).as<std::string>();
  if (keys_path_.empty() || !fs::exists(keys_path_)) {
    LOG(kError) << "Incorrect information in parameter " << option_name;
    throw std::exception();
  }

  LOG(kInfo) << "GetPathFromProgramOption - " << option_name << " is " << keys_path_;
}

boost::asio::ip::udp::endpoint Commander::GetBootstrapEndpoint(const std::string& peer) {
  size_t delim = peer.rfind(':');
  boost::asio::ip::udp::endpoint ep;
  ep.port(boost::lexical_cast<uint16_t>(peer.substr(delim + 1)));
  ep.address(boost::asio::ip::address::from_string(peer.substr(0, delim)));
  LOG(kInfo) << "Going to bootstrap off endpoint " << ep;
  return ep;
}

po::options_description Commander::AddGenericOptions(const std::string& title) {
  po::options_description generic_options(title);
  generic_options.add_options()
      ("help,h", "Print this help message.")
      ("create,c", "Create keys and write to file.")
      ("load,l", "Load keys from file.")
      ("run,r", "Run vaults with available keys.")
      ("bootstrap,b", "Run boostrap nodes only, using first 2 keys.")
      ("store,s", "Store keys on network.")
      ("verify,v", "Verify keys are available on network.")
      ("test,t", "Run simple test that stores and retrieves chunks.")
//      ("extended,x", "Run extended test that tries all operations on all chunk types.")
      ("delete,d", "Delete keys file.")
      ("print,p", "Print the list of keys available.");
  return generic_options;
}

po::options_description Commander::AddConfigurationOptions(const std::string& title) {
  po::options_description config_file_options(title);
  boost::system::error_code error_code;
  config_file_options.add_options()
      ("peer",
       po::value<std::string>(),
       "Endpoint of bootstrap node, if attaching to running network.")
      ("pmids_count,n",
       po::value<size_t>(&pmids_count_)->default_value(pmids_count_),
       "Number of keys to create")
      ("keys_path",
       po::value<std::string>()->default_value(
           fs::path(fs::temp_directory_path(error_code) / "key_directory.dat").string()),
       "Path to keys file")
      ("chunk_path",
       po::value<std::string>()->default_value(
           fs::path(fs::temp_directory_path(error_code) / "keys_chunks").string()),
       "Path to chunk directory")
      ("chunk_set_count",
       po::value<size_t>(&chunk_set_count_)->default_value(chunk_set_count_),
       "Number of chunk sets to run extended test on");
  return config_file_options;
}

po::variables_map Commander::CheckOptionValidity(po::options_description& cmdline_options,
                                                 int argc,
                                                 char* argv[]) {
  po::command_line_parser parser(argc, argv);
  po::variables_map variables_map;
  po::store(parser.options(cmdline_options).allow_unregistered().run(), variables_map);
  po::notify(variables_map);
  selected_ops_.do_create = variables_map.count("create") != 0;
  selected_ops_.do_load = variables_map.count("load") != 0;
  selected_ops_.do_run = variables_map.count("run") != 0;
  selected_ops_.do_bootstrap = variables_map.count("bootstrap") != 0;
  selected_ops_.do_store = variables_map.count("store") != 0;
  selected_ops_.do_verify = variables_map.count("verify") != 0;
  selected_ops_.do_test = variables_map.count("test") != 0;
  selected_ops_.do_extended = variables_map.count("extended") != 0;
  selected_ops_.do_delete = variables_map.count("delete") != 0;
  selected_ops_.do_print = variables_map.count("print") != 0;

  auto conflicting_options = [this] () {  return  !selected_ops_.do_create &&
                                                  !selected_ops_.do_load &&
                                                  !selected_ops_.do_run &&
                                                  !selected_ops_.do_bootstrap &&
                                                  !selected_ops_.do_store &&
                                                  !selected_ops_.do_verify &&
                                                  !selected_ops_.do_test &&
                                                  !selected_ops_.do_extended &&
                                                  !selected_ops_.do_delete &&
                                                  !selected_ops_.do_print;
                                       };
  if (variables_map.count("help") || conflicting_options()) {
    std::cout << cmdline_options << ": Options order: [c|l|d] p [r|b] s v t" << std::endl;
    throw std::exception();
  }
  return variables_map;
}

void Commander::CreateKeys() {
  all_pmids_.clear();
  for (size_t i = 0; i < pmids_count_; ++i) {
    passport::Anmaid anmaid;
    passport::Maid maid(anmaid);
    passport::Pmid pmid(maid);
    all_pmids_.push_back(pmid);
  }
  LOG(kInfo) << "Created " << all_pmids_.size() << " pmids.";
  if (maidsafe::passport::detail::WritePmidList(keys_path_, all_pmids_)) {
    LOG(kInfo) << "Wrote keys to " << keys_path_;
  } else {
    LOG(kError) << "Could not write keys to " << keys_path_;
    throw std::exception();
  }
}

void Commander::HandleKeys() {
  if (selected_ops_.do_create) {
    CreateKeys();
  } else if (selected_ops_.do_load) {
    all_pmids_ = maidsafe::passport::detail::ReadPmidList(keys_path_);
    LOG(kInfo) << "Loaded " << all_pmids_.size() << " pmids from " << keys_path_;
  }

  if (selected_ops_.do_print) {
    for (size_t i(0); i < all_pmids_.size(); ++i)
      LOG(kInfo) << '\t' << i << "\t PMID " << HexSubstr(all_pmids_.at(i).name().data)
                 << (i < 2 ? " (bootstrap)" : "");
  }
}

void Commander::HandleNetWork() {
  assert(all_pmids_.size() >= 4);
  if (selected_ops_.do_run || selected_ops_.do_bootstrap)
    SetupNetwork(all_pmids_, !selected_ops_.do_run && selected_ops_.do_bootstrap);
}

bool Commander::HandleStore() {
  if (selected_ops_.do_store) {
    try {
      KeyStorer storer(peer_endpoints_);
      storer.Store(all_pmids_);
      return true;
    }
    catch(...) {
      return false;
    }
  }
  return false;
}

bool Commander::HandleVerify() {
  if (selected_ops_.do_verify) {
    try {
      KeyVerifier verifier(peer_endpoints_);
      verifier.Verify(all_pmids_);
      return true;
    }
    catch(...) {
      return false;
    }
  }
  return false;
}

bool Commander::HandleDoTest() {
  if (selected_ops_.do_test) {
    assert(all_pmids_.size() >= 4);
    try {
      DataChunkStorer chunk_storer(peer_endpoints_);
      return chunk_storer.Test();
    }
    catch(...) {
      return false;
    }
  }
  return false;
}

void Commander::HandleDeleteKeys() {
  if (selected_ops_.do_delete) {
    if (fs::remove(keys_path_))
      LOG(kInfo) << "Deleted " << keys_path_;
    else
      LOG(kError) << "Could not delete " << keys_path_;
  }
}

}  // namespace tools

}  // namespace vault

}  // namespace maidsafe
