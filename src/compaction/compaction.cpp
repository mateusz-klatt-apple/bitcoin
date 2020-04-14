#include "compaction/params.h"
#ifdef COMSYS_COMPACTION

#include "compaction.h"
#include "evaluation.h"

#ifdef ENABLE_COMPACTION

#include <rpc/server.h>
#include <netmessagemaker.h>
#include <validation.h>
#include <consensus/validation.h>
#include <txdb.h>
#include <coins.h>
#include <logging.h>
#include <shutdown.h>
#include <chain.h>
#include <init.h>
#include <iomanip>
#include <primitives/transaction.h>
#include <streams.h>

// global variables
bool provideState = false;
bool full_sync_mode = false;
unsigned int header_chain_best_known = 0;
bool syncComplete = false;
bool overloadingState = false;
// Signal if we are loading a compaction state to chainstate DB
// Not all possible checks are being made, we rather assume that
// only RPC calls can cause multiple states to be attempted to
// be loaded simultaneously.
bool in_state_loading_phase = false;
NodeStateStatus nodeStatus[MAX_OUTBOUND_CONNECTIONS];
std::unique_ptr<CompactionState> currentState = nullptr;
std::unique_ptr<CompactionState> prevState = nullptr;
std::unique_ptr<CompactionState> downloaded_state = nullptr;

std::map<uint256, std::vector<CNode*>> mapOfferedStates;
std::map<uint256, std::vector<uint256>> mapStateChunks;
std::map<uint256, ChunkStatus> mapChunkStatus;
std::map<CNode*, std::string> mapNodeToChunk;
uint256 requestedState;
std::string requestedStateFilename;
int64_t compaction_coindbcache = (1 << 23);

// local constants
const char* confirmationTag = "CoinPrune";
const char* confirmationSeparator = "/";

// local variables
std::map<uint256, unsigned int> confirmations;

bool CompactionState::want_to_create_state = false;
bool CompactionState::can_create_state = false;
unsigned int CompactionState::desired_state_height = 0;
std::unique_ptr<CompactionState>* CompactionState::desired_state_target = nullptr;
std::unique_ptr<CompactionState>* CompactionState::desired_state_previous = nullptr;
bool CompactionState::will_create_dummy_state = false;
bool CompactionState::communication_paused = false;
bool CompactionState::blocks_in_flight = true;  // At least potentially, unless we explicitly set them not to be

// RPC command array
static const CRPCCommand rpcCommands[] = {
    { "control", "createstate", &createState, { "maxHeight" } },
    { "control", "loadstate", &loadState, { "maxHeight" } },
    { "control", "overloadprevstate", &overloadPrevState, { "maxHeight" } },
    { "control", "createevalstates", &createEvalStates, { "stepSize", "numberStates", "fromEnd" } },
    { "control", "readytoserve", &isReadyToServe, {} },
};

void registerCompactionRPCCommands(CRPCTable &t) {
    t.appendCommand(rpcCommands[0].name, &rpcCommands[0]);
    t.appendCommand(rpcCommands[1].name, &rpcCommands[1]);
    t.appendCommand(rpcCommands[2].name, &rpcCommands[2]);
    t.appendCommand(rpcCommands[3].name, &rpcCommands[3]);
    t.appendCommand(rpcCommands[4].name, &rpcCommands[4]);
}

// RPC function
UniValue createState(const JSONRPCRequest& request) {
    // display help message if necessary
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "createstate \"maxHeight\"\n"
                "\nCreate a UTXO-state file in the current data directory.\n"
                "\nArguments:\n"
                "1. \"maxHeight\"     (int, required) The height of the last block that shall be considered or 0 to include all blocks\n");
    
    if (IsStateCurrentlyLoading()) {
        std::string result = "State is currently being loaded. Aborting.\n";
        return result;
    }

    // extract maximum block height from parameter
    int height = -1;
    std::stringstream(request.params[0].get_str()) >> height;

    // create state
    CompactionState::setWantToCreateState(height, &dummy_compaction_state, nullptr, true);
#ifdef ENABLE_EVALUATION
    eval_last_state_height = height;
#endif

    // return success message
    std::string result = "Triggered state writing of height '";
    result += height;
    result += "'\r\n";
    return result;
}

// RPC function
UniValue loadState(const JSONRPCRequest& request) {
    // display help message if necessary
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error("loadstate \"file\"\n"
                "\nLoads the given state file and applies it to the UTXO database.\n"
                "The state file must be located in the current data directory.\n"
                "\nArguments:\n"
                "1. \"file\"     (string, required) file name of the state to be loaded");

    if (IsStateCurrentlyLoading()) {
        std::string result = "Another state is currently being loaded. Aborting.";
        return result;
    }

    // load state
    std::unique_ptr<CompactionState> state(CompactionState::loadState(request.params[0].get_str()));

    // apply to UTXO database
    state->loadToChainStateDatabase();

    // return success message
    std::string result = "Successfully read state from file '" + state->getFileName() + "'\r\n";
    result += "Hash: " + state->getHash().ToString();
    state.reset();
    return result;
}

// RPC function
UniValue overloadPrevState(const JSONRPCRequest& request) {
    // display help message if necessary
#ifndef ENABLE_EVALUATION
    throw std::runtime_error("overloadprevstate \"file\"\n"
            "\nOverload prevState with the state file for the given height. It does NOT apply it to the UTXO database (eval only).\n"
            "The state file must be located in the current data directory.\n"
            "\nArguments:\n"
            "1. \"file\"     (string, required) height of the state file to be loaded");
#else
    if (request.fHelp || request.params.size() == 0 || request.params.size() > 2)
        throw std::runtime_error("overloadprevstate \"file\" [tail_length]\n"
                "\nOverload prevState with the state file for the given height. It does NOT apply it to the UTXO database (eval only).\n"
                "The state file must be located in the current data directory.\n"
                "\nArguments:\n"
                "1. \"file\"      (string, required) height of the state file to be loaded"
                "2. [tail_length] (uint, optional) Update tail length");

    if (IsStateCurrentlyLoading()) {
        std::string result = "Another state is currently being loaded. Aborting.";
        return result;
    }

    overloadingState = true;
    std::stringstream s;
    s << std::setfill('0') << std::setw(10) << request.params[0].get_str();
    std::string fileName = s.str() + ".state";

    // load state
    prevState = CompactionState::loadState(fileName);
    if (currentState) {
        currentState->setPrevious(prevState);
    }

    // return success message
    std::string result = "Successfully overloaded prevState from file '" + prevState->getFileName() + "'\n";
    result += "Hash:   " + prevState->getHash().ToString() + "\n";
    result += "Height: " + prevState->getHeight();
    result += "\n";

    if (request.params.size() == 2) {
        std::stringstream(request.params[1].get_str()) >> eval_tail_length;
        result += "Also updated tail length: " + eval_tail_length;
        result += "\n";
    }

    eval_state_height = prevState->getHeight();
    overloadingState = false;
    return result;
#endif
}

// RPC function
UniValue createEvalStates(const JSONRPCRequest& request) {
#ifndef ENABLE_EVALUATION
    throw std::runtime_error(
            "createstate \"stepSize\" \"numberStates\" \"fromEnd\"\n"
            "\nCreate multiple UTXO-state files in the current data directory for evaluation purposes (eval only).\n"
            "\nNOTE: Shuts down bitcoind automatically after concluding!\n"
            "\nArguments:\n"
            "1. \"stepSize\"     (int, required) The step size for state creation.\n"
            "2. \"numberStates\" (int, required) The number of states to produce; set to 0 for maximum number of states until genesis block.\n"
            "3. \"fromEnd\"      (int, required) Set to 1 to start creating states from the end.\n"
            );
#else
    // display help message if necessary
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
                "createstate \"stepSize\" \"numberStates\" \"fromEnd\"\n"
                "\nCreate multiple UTXO-state files in the current data directory for evaluation purposes (eval only).\n"
                "\nNOTE: Shuts down bitcoind automatically after concluding!\n"
                "\nArguments:\n"
                "1. \"stepSize\"     (int, required) The step size for state creation.\n"
                "2. \"numberStates\" (int, required) The number of states to produce; set to 0 for maximum number of states until genesis block.\n"
                "3. \"fromEnd\"      (int, required) Set to 1 to start creating states from the end.\n"
        );

    if (IsStateCurrentlyLoading()) {
        std::string result = "Another state is currently being loaded. Aborting.";
        return result;
#endif
    }

    // extract maximum block height from parameter
    unsigned int step_size = 0;
    std::stringstream(request.params[0].get_str()) >> step_size;
    unsigned int number_states = 0;
    std::stringstream(request.params[1].get_str()) >> number_states;
    unsigned int from_end = 0;
    std::stringstream(request.params[2].get_str()) >> from_end;

    // create state
    CompactionState::createEvalStates(step_size, number_states, (from_end != 0) ? true : false);

    // return success message
    std::string result = "Successfully wrote states. Shutting down now.";
    return result;
}

// RPC function
UniValue isReadyToServe(const JSONRPCRequest& request) {
#ifndef ENABLE_EVALUATION
    throw std::runtime_error(
            "readytoserve\n"
            "\nBoolean check whether or not a server is ready to serve information (eval only).\n"
            );
#else
    // display help message if necessary
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
                "readytoserve\n"
                "\nBoolean check whether or not a server is ready to serve information (eval only).\n"
        );

    CChain& chainActive = g_chainstate.chainActive;
    bool sufficient_active_chain = (chainActive.Height() >= prevState->getHeight() + eval_tail_length);

    // return success message
    std::string result = "readytoserve: ";
    result += ((syncComplete && sufficient_active_chain && !overloadingState) ? "true" : "false");
    result += "\n";
    return result;
#endif
}


void initializeCompaction() {

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Initializing compaction mode.\n", __FILE__, __func__, __LINE__);
    // check whether we are already synchronized
    LOCK(cs_main);
    syncComplete = (chainActive.Height() >= initialStateHeight) ? true : false;
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Synchronization is %s complete on this peer.\n", __FILE__, __func__, __LINE__, ((syncComplete) ? "already" : "NOT"));

    // set state status for all nodes to NOT_CONNECTED
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Initializing state status table for maximum %d outgoing connections.\n", __FILE__, __func__, __LINE__, MAX_OUTBOUND_CONNECTIONS);
    for (int i = 0; i < MAX_OUTBOUND_CONNECTIONS; i++) {
        nodeStatus[i] = NOT_CONNECTED;
    }
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Set all %d state status slots to NOT_CONNECTED.\n", __FILE__, __func__, __LINE__, MAX_OUTBOUND_CONNECTIONS);

    // Create statedir
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Checking for /compaction_states/ subfolder in datadir.\n", __FILE__, __func__, __LINE__);
    if (!boost::filesystem::exists(GetStateDir())){
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Subfolder did not exist, creating it.\n", __FILE__, __func__, __LINE__);
        boost::filesystem::create_directory(GetStateDir());
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Subfolder already existed. Doing nothing here.\n", __FILE__, __func__, __LINE__);
    }

    // Create chunkdir
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Checking for /chunks/ subfolder in state directory.\n", __FILE__, __func__, __LINE__);
    if (!boost::filesystem::exists(GetStateDir() + "chunks/")){
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Subfolder did not exist, creating it.\n", __FILE__, __func__, __LINE__);
        boost::filesystem::create_directory(GetStateDir() + "chunks/");
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Subfolder already existed. Doing nothing here.\n", __FILE__, __func__, __LINE__);
    }

    // check whether we have to request a state or whether it is provided
    if (!syncComplete) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Since synchronization was not yet complete, I try to synchronize!\n", __FILE__, __func__, __LINE__);
        if (gArgs.IsArgSet("-compaction")) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Using COMSYS compaction for fast synchronization.\n", __FILE__, __func__, __LINE__);
            // check whether UTXO database is empty
            if (pcoinsTip->GetBestBlock() != uint256()) {
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: CRITICAL: UTXO database is not empty at synchronization start up! Shutting down.\n", __FILE__, __func__, __LINE__);
                StartShutdown();
                return;
            }
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: UTXO database is empty, so we are ready to go.\n", __FILE__, __func__, __LINE__);

            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Checking whether to fast-synchronize via Bitcoin network or locally.\n", __FILE__, __func__, __LINE__);
            std::string fileName = gArgs.GetArg("-statename", "");
            if (fileName.empty()) {
                // state will be requested when receiving verack message
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: No state provided with -statename, trying to download it from peers\n", __FILE__, __func__, __LINE__);
            }
            else {
                // state file provided by parameter
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: State provided with -statename, loading..\n", __FILE__, __func__, __LINE__);
                currentState = std::move(CompactionState::loadState(fileName));
            }
        }
        else {
            switchToFullSync(nullptr, 0);
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Slow synchronization, consider restarting with -compaction!\n", __FILE__, __func__, __LINE__);
        }
    }
    else {
        switchToFullSync(nullptr, 0);
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Node is already synchronized!\n", __FILE__, __func__, __LINE__);
    }

    if (gArgs.IsArgSet("-provideState")) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I am going to serve states to requesting peers.\n", __FILE__, __func__, __LINE__);
        provideState = true;

#ifdef ALWAYS_PROVIDE_STATE
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I shall ALWAYS provide my state.\n", __FILE__, __func__, __LINE__);
        syncComplete = true;

        std::stringstream fileName;
        fileName << std::setfill('0') << std::setw(10) << initialStateHeight;
        std::string name = fileName.str() + ".state";
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Loading state file: %s.\n", __FILE__, __func__, __LINE__, GetStateDir() + name);
        if (boost::filesystem::exists(GetStateDir() + name)) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: State %s exists on disk.\n", __FILE__, __func__, __LINE__, name);
            currentState = std::move(CompactionState::loadState(name));
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Loaded state %s from disk.\n", __FILE__, __func__, __LINE__, name);
        } else {
            StartShutdown();
            return;
            initializeState();
        }

        prevState = std::move(currentState);
#  ifdef ENABLE_EVALUATION
        eval_state_height = prevState->getHeight();
#  endif

#else
        // create state object if enough blocks available
        if (syncComplete) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Initializing state as I know it now.\n", __FILE__, __func__, __LINE__);
            initializeState();
        }
#endif

    }
    requestedState.SetNull();
}


bool switchToFullSync(std::unique_ptr<CompactionState>* downloaded_state, unsigned int header_chain_height) {
    // We only switch to full sync once
    if (full_sync_mode) {
        return false;
    }

    // switching to full sync on a nullptr means we want to use legacy synchronization
    // in that case, we only set the flag, and only for a valid downloaded state, we
    // now apply that.
    if (downloaded_state == nullptr) {
        full_sync_mode = true;
        return full_sync_mode;
    }

    // if our header chain is insufficien, we cannot switch yet
    if (!(*downloaded_state) || header_chain_height < (*downloaded_state)->getHeight()) {
        return false;
    }

    // We're all set and can apply our state

    // Move downloaded state to be our transient state now
    currentState = std::move(*downloaded_state);
    // Apply state UTXO information to chainstate db
    currentState->loadToChainStateDatabase();

    full_sync_mode = true;
    return full_sync_mode;
}


void requestStateFrom(CNode* node) {
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Requesting state from peer %d (%s)\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
    CSerializedNetMsg message = CNetMsgMaker(node->GetSendVersion()).Make(NetMsgType::GETSTATE);
    g_connman->PushMessage(node, (CSerializedNetMsg&&)message);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Sent GETSTATE to peer\n", __FILE__, __func__, __LINE__);

    if (node->GetId() < MAX_OUTBOUND_CONNECTIONS) {
        if (nodeStatus[node->GetId()] != NOT_REQUESTED) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Tried to request state from peer %d (%s) who is *NOT* in connected state.\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
            return;
        }
        nodeStatus[node->GetId()] = REQUESTED;
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Set internal state table entry of peer %d (%s) to REQUESTED.\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Did not change internal state table as peer id %d (%s) is larger than MAX_OUTBOUND_CONNECTIONS (%d)\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName(), MAX_OUTBOUND_CONNECTIONS);
        return;
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Done requesting state from peer %d (%s), now waiting for response.\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
}

void requestStateChunksFrom(CNode* node, unsigned int number_chunks) {
    unsigned int effectively_requested_number_chunks = 0;
    if (number_chunks == 0 || number_chunks >= MAX_DOWNLOADS_PER_PEER) {
        effectively_requested_number_chunks = MAX_DOWNLOADS_PER_PEER;
    } else {
        effectively_requested_number_chunks = number_chunks;
    }

    if (node->number_requested_state_chunks >= effectively_requested_number_chunks) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Not downloading new chunks from peer %d (%s), maxed out.\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
        return;
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Going to download the next %d NEEDED state chunks from peer %d (%s).\n", __FILE__, __func__, __LINE__, (effectively_requested_number_chunks - node->number_requested_state_chunks), node->GetId(), node->GetAddrName());
    }

    std::vector<CInv> vCInv;
    for (auto const& chunk: mapStateChunks[requestedState]) {
        if (node->number_requested_state_chunks >= effectively_requested_number_chunks) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Reached maximum number of downloads for peer %d (%s) for now.\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
            break;
        }
        if (mapChunkStatus[chunk] != NEEDED) {
            continue;
        }
        vCInv.push_back(CInv(MSG_STATE, chunk));
        mapChunkStatus[chunk] = IN_TRANSIT;
        node->number_requested_state_chunks += 1;
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Going to request state chunk %s from peer %d (%s).\n", __FILE__, __func__, __LINE__, chunk.ToString(), node->GetId(), node->GetAddrName());
    }

    if (vCInv.size() > 0) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Sending GETDATA request to peer %d (%s). Content (Length %d):\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName(), vCInv.size());
        for (auto const& chunk: vCInv) {
            LogPrint(BCLog::COMPACTION, "    %s\n", chunk.ToString());
        }
        CSerializedNetMsg message = CNetMsgMaker(node->GetSendVersion()).Make(NetMsgType::GETDATA, vCInv);
        g_connman->PushMessage(node, (CSerializedNetMsg&&) message);
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Did not find new chunks to obtain from peer %d (%s).\n", __FILE__, __func__, __LINE__, node->GetId(), node->GetAddrName());
    }
}

bool checkConnection(CNode* node) {
    // This check is irrelevant for our evaluation.
    return true;
}

void initializeState() {
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Starting to initialize received state.\n", __FILE__, __func__, __LINE__);
    int chainHeight;
    {
        LOCK(cs_main);
        chainHeight = chainActive.Height();
    }

    uint256 hash;
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Searching for last confirmed state..\n", __FILE__, __func__, __LINE__);
    int stateHeight = searchLastConfirmedState(hash);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Found last confirmed state on block height %d\n", __FILE__, __func__, __LINE__, stateHeight);

    if (chainHeight >= stateHeight) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Creating local presentation of confirmed state.\n", __FILE__, __func__, __LINE__);
        // Here, we don't have communication going yet, so we can just create a state
        currentState = std::move(CompactionState::createState(stateHeight));
        if (hash != currentState->getHash() && initialStateHeight != currentState->getHeight()) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: The confirmed state does not match with our state! Shutting down.\n", __FILE__, __func__, __LINE__);
            StartShutdown();
            return;
        } else {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Our state is compatible to the confirmed state, so we can accept it!\n", __FILE__, __func__, __LINE__);
        }
    }
}

// returns the hash of the given file
uint256 calculateHashFromFile(const std::string& file) {
    // open file and get its size
    std::fstream f(file, std::ios_base::in | std::ios_base::binary);
    f.seekg(0, f.end);
    long fileSize = f.tellg();
    f.seekg(0, f.beg);

    // create hashStream
    CHashWriter hashStream(SER_GETHASH, PROTOCOL_VERSION);

    // while there is data to read, hash it
    while (fileSize > 0) {
        // copy at most 1MB at once
        int bytesToCopy = (fileSize < 1024 * 1024) ? fileSize : 1024 * 1024;
        char buffer[1024 * 1024];
        f.read(buffer, bytesToCopy);
        hashStream.write(buffer, bytesToCopy);
        fileSize -= bytesToCopy;

        // allow interruption, because the loop can take some time
        boost::this_thread::interruption_point();
    }

    return hashStream.GetHash();
}

uint256 calculateHashFromString(const std::string& input_string) {
    // Get size of input string
    long string_size = input_string.length();

    // create hashStream
    CHashWriter hashStream(SER_GETHASH, PROTOCOL_VERSION);

    // while there is data to read, hash it
    long bytes_copied = 0;
    long bytes_left = 0;
    while (bytes_left > 0) {
        bytes_left = string_size - bytes_copied;
        // copy at most 1MB at once
        long bytesToCopy = (bytes_left < 1024 * 1024) ? bytes_left : 1024 * 1024;
        char buffer[1024 * 1024];
        strncpy(buffer, input_string.substr(bytes_copied, bytesToCopy).c_str(), bytesToCopy);
        hashStream.write(buffer, bytesToCopy);
        bytes_copied += bytesToCopy;

        // allow interruption, because the loop can take some time
        boost::this_thread::interruption_point();
    }

    return hashStream.GetHash();
}

uint256 CompactionState::calculateStateHash(){

    std::vector<uint256> vHashes;
    vHashes.push_back(stateFileHash);
    for (size_t i = 0; i < chunks.size(); i++) {
        vHashes.push_back(chunks.at(i)->chunkHash);
    }

    return Hash(vHashes.begin(), vHashes.end());
}

void addConfirmationToCoinbaseScript(CScript& script) {

    // nothing to confirm yet
    if (!currentState) {
        return;
    }

    // write hash to char array
    char hashBytes[32];
    for (int i=0;i<4;i++) {
        uint64_t tmp = currentState->getHash().GetUint64(i);
        memcpy(&hashBytes[i * 8], &tmp, 8);
    }

    // resize script
    int tagSize = strlen(confirmationTag);
    int separatorSize = strlen(confirmationSeparator);
    int previousSize = script.size();
    int completeTagSize = tagSize + (2 * separatorSize) + 32;
    int newSize = previousSize + completeTagSize;
    script.resize(newSize);

    // move old script to make space for confirmation
    // (truncate end if necessary)
    for (int i = 0; i < previousSize && i < (100 - completeTagSize); i++) {
        script[completeTagSize + i] = script[i];
    }

    // prepend tag to script
    for (int i = 0; i < tagSize; i++) {
        script[i] = confirmationTag[i];
    }

    // append separator 1 to script
    for (int i = 0; i < separatorSize; i++) {
        script[tagSize + i] = confirmationSeparator[i];
    }

    // append hash to script
    for (int i = 0; i < 32; i++) {
        script[tagSize + separatorSize + i] = hashBytes[i];
    }

    // append separator 2 to script
    for (int i = 0; i < separatorSize; i++) {
        script[tagSize + separatorSize + 32 + i] = confirmationSeparator[i];
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: wrote script to coinbase!\n", __FILE__, __func__, __LINE__);
}

void handleNewBlock(std::shared_ptr<const CBlock> block, const CBlockIndex* blockIndex) {
    // We do not actually have a block to handle yet
    if (!block) {
        return;
    }

    uint256 hash;
    if (checkForStateConfirmation(block, hash)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Found confirmation for state %s\n", __FILE__, __func__, __LINE__, hash.ToString());
        addConfirmation(hash);

        // if our current state is confirmed, create a new one
        if (isConfirmed(hash)) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: State %s is sufficiently confirmed\n", __FILE__, __func__, __LINE__, hash.ToString());

            // check whether we have the correct state
            if (currentState->getHash() != hash) {
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: \nWARNING\nCurrent state does not match with confirmed one!\nConsider resynchronizing!\n\n", __FILE__, __func__, __LINE__);
                StartShutdown();
                return;
            }

            // remove previous state
            if (prevState) {
                boost::filesystem::remove(prevState->getFileName());
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Removed deprecated state file %s\n", __FILE__, __func__, __LINE__, prevState->getFileName());
            }

            // move current to previous
            currentState->setConfirmed();
            currentState->resetPrevious();
            prevState = std::move(currentState);

            // create new state
            CompactionState::setWantToCreateState(blockIndex->nHeight, &currentState, &prevState, true);
#ifdef ENABLE_EVALUATION
            eval_last_state_height = blockIndex->nHeight;
#endif

            // remove deprecated block files (only has effect if started with -prune=1)
            // keep blocks after prevState to keep its confirmations
            if (fPruneMode) {
                PruneBlockFilesManual(prevState->getHeight());
            }

            syncComplete = true;
        }
    }

}

bool checkForStateConfirmation(std::shared_ptr<const CBlock> block, uint256& hash) {
    // it is possible that block is not initialized properly
    if (block->vtx.size() == 0 || block->vtx[0]->vin.size() == 0) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Invalid block, not searching for confirmation\n", __FILE__, __func__, __LINE__);
        return false;
    }
    int pos = searchConfirmationStart(block->vtx[0]->vin[0].scriptSig);
    if (pos == -1) {
        return false;
    }
    else {
        // extract hash
        std::vector<unsigned char> hashBytes;
        for (unsigned int i = 0; i < 32; ++i) {
            hashBytes.push_back(block->vtx[0]->vin[0].scriptSig[pos + strlen(confirmationSeparator) + i]);
        }

        // dont miss this function call, otherwise the next line will cause a crash without any error message
        hashBytes.shrink_to_fit();
        hash = uint256(hashBytes);

        return true;
    }
}

int searchConfirmationStart(const CScript& script) {
    unsigned int tagPosition = 0;
    unsigned int tagSize = strlen(confirmationTag);

    for (unsigned int i = 0; i < script.size(); ++i) {
        if (script[i] == confirmationTag[tagPosition]) {
            tagPosition++;
            if (tagPosition == tagSize) {
                return i + 1;
            }
        }
        else {
            i -= tagPosition;
            tagPosition = 0;
        }
    }

    return -1;
}

void addConfirmation(const uint256& stateHash) {
    // initialize with 0 if new state is added
    if (confirmations.find(stateHash) == confirmations.end()) {
        confirmations[stateHash] = 0;
    }

    ++confirmations[stateHash];
}

bool isConfirmed(const uint256& stateHash) {
    if (confirmations.find(stateHash) == confirmations.end()) {
        return false;
    }
    else {
        return confirmations[stateHash] >= requiredConfirmations;
    }
}

int searchLastConfirmedState(uint256& hash) {
    LOCK(cs_main);
    confirmations.clear();
    CBlock block;
    auto pblock = std::make_shared<CBlock>(block);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Searching for last confirmed state (from within function).", __FILE__, __func__, __LINE__);
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && static_cast<unsigned int>(pindex->nHeight) >= initialStateHeight; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Could not read block from disk!\n", __FILE__, __func__, __LINE__);
        }

        if (checkForStateConfirmation(pblock, hash)) {
            addConfirmation(hash);
        }

        if (isConfirmed(hash)) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Last confirmed state is %s\n", __FILE__, __func__, __LINE__, hash.ToString());
            return pindex->nHeight;
        }
    }

    // if no state is sufficiently confirmed
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: No confirmed state found!\n", __FILE__, __func__, __LINE__);
    return initialStateHeight;
}

void rewindUTXOTo(unsigned int height, std::vector<CBlockIndex*>& undoneBlocks, std::unique_ptr<CCoinsViewCompaction>& pcoins) {
    LOCK(cs_main);
    unsigned int latestBlockHeight = mapBlockIndex[pcoins->GetBestBlock()]->nHeight;
    CCoinsViewCache *pcoinsbase = pcoins.get();

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Rewinding UTXO to height %d, current height: %d\n", __FILE__, __func__, __LINE__, height, latestBlockHeight);

    for (unsigned int i = latestBlockHeight; i > height; i--) {
        CBlockIndex* currentBlockIndex = chainActive[i];
        undoneBlocks.push_back(currentBlockIndex);

        CBlock currentBlock;
        ReadBlockFromDisk(currentBlock, currentBlockIndex, Params().GetConsensus());

        DisconnectResult disconnect_result = g_chainstate.DisconnectBlock(currentBlock, currentBlockIndex, *pcoinsbase);
        if (disconnect_result == DISCONNECT_FAILED) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: CRITICAL! DisconnectBlock was not successful for block on height %d.\n", __FILE__, __func__, __LINE__, i);
        } else if (disconnect_result == DISCONNECT_FAILED) {
            LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: DisconnectBlock was unclean for block on height %d.\n", __FILE__, __func__, __LINE__, i);
        } else {
            LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: DisconnectBlock was successful for block on height %d; now %u UTXO elements.\n", __FILE__, __func__, __LINE__, i, pcoins->cacheCoinsOrdered.size());
        }

        if (i % 5000 == 0) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Progess, current height: %d\n", __FILE__, __func__, __LINE__, i);
        }
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Done rewinding.\n", __FILE__, __func__, __LINE__);
}

void rewindUTXOToAndForget(unsigned int height, std::unique_ptr<CCoinsViewCompaction>& pcoins) {
    LOCK(cs_main);
    unsigned int latestBlockHeight = mapBlockIndex[pcoins->GetBestBlock()]->nHeight;
    CCoinsViewCache *pcoinsbase = pcoins.get();

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Rewinding UTXO to height %d, current height: %d\n", __FILE__, __func__, __LINE__, height, latestBlockHeight);

    for (unsigned int i = latestBlockHeight; i > height; i--) {
        CBlockIndex* currentBlockIndex = chainActive[i];

        CBlock currentBlock;
        ReadBlockFromDisk(currentBlock, currentBlockIndex, Params().GetConsensus());

        DisconnectResult disconnect_result = g_chainstate.DisconnectBlock(currentBlock, currentBlockIndex, *pcoinsbase);
        if (disconnect_result == DISCONNECT_FAILED) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: CRITICAL! DisconnectBlock was not successful for block on height %d.\n", __FILE__, __func__, __LINE__, i);
        } else if (disconnect_result == DISCONNECT_FAILED) {
            LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: DisconnectBlock was unclean for block on height %d.\n", __FILE__, __func__, __LINE__, i);
        } else {
            LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: DisconnectBlock was successful for block on height %d; now %u UTXO elements.\n", __FILE__, __func__, __LINE__, i, pcoins->cacheCoinsOrdered.size());
        }

        if (i % 5000 == 0) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Progess, current height: %d\n", __FILE__, __func__, __LINE__, i);
        }
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Done rewinding.\n", __FILE__, __func__, __LINE__);
}

void toByteVector(const std::string& fileName, std::vector<unsigned char>& v) {
    // open the file
    std::fstream f(fileName, std::ios_base::in | std::ios_base::binary);

    // get file length
    f.seekg(0, f.end);
    long fileSize = f.tellg();
    f.seekg(0, f.beg);

    // copy file to vector
    while (fileSize > 0) {
        // copy at most 1MB at once
        int bytesToCopy = (fileSize < 1024 * 1024) ? fileSize : 1024 * 1024;
        unsigned char buffer[1024 * 1024];
        f.read(reinterpret_cast<char*>(buffer), bytesToCopy);

        v.insert(v.end(), &buffer[0], &buffer[bytesToCopy]);
        fileSize -= bytesToCopy;

        // allow interruption, because the loop can take some time
        boost::this_thread::interruption_point();
    }

    f.close();
}

unsigned int CompactionState::serializeChunkFile(std::vector<std::pair<COutPoint, Coin>>& utxo_buffer, std::unique_ptr<CompactionState>& state, unsigned long chunk_offset) {
    std::unique_ptr<CompactionChunk> chunk(new CompactionChunk);
    chunk->height = state->height;
    chunk->offset = chunk_offset;
    chunk->fileName = createChunkFileName(chunk->height, chunk->offset);

    // open new chuck file, named after the max block height and offset
    CAutoFile chunk_file(fsbridge::fopen(chunk->fileName, "wb"), SER_DISK, CLIENT_VERSION);

    // write chunk height and offset to file
    chunk_file << chunk->height;
    chunk_file << chunk->offset;

    Serialize(chunk_file, utxo_buffer);

    // calculate the hash and push the chunk to the chunks vector
    chunk->chunkHash = calculateHashFromFile(chunk->fileName);

    state->mapHashToChunk[chunk->chunkHash] = chunk_offset;
    state->chunks.push_back(std::move(chunk));

    // close the chunk file
    chunk_file.fclose();

    // increase the chunk offset
    return chunk_offset + 1;
}

void CompactionState::serializeStateFile(std::unique_ptr<CompactionState>& state, std::unique_ptr<CCoinsViewCompaction>& pcoins) {

    // Variables for chunk creation
    unsigned long offset = 0;
    unsigned long overhead_size = ::GetSerializeSize(state->height, SER_DISK, CLIENT_VERSION) + ::GetSerializeSize(offset, SER_DISK, CLIENT_VERSION);

    try {
        // iterate over all utxos in UTXO set
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Starting to write out chunks.\n", __FILE__, __func__, __LINE__);

        std::pair<COutPoint, Coin> utxo_element;
        std::vector<std::pair<COutPoint, Coin>> utxo_buffer;
        unsigned int debug_ctr = 0;
        for (CCoinsMapOrdered::iterator it = pcoins->cacheCoinsOrdered.begin(); it != pcoins->cacheCoinsOrdered.end(); ++it) {
            utxo_element.first = it->first;
            utxo_element.second = it->second;
            // check whether transaction is old enough to be contained in state
            if (static_cast<unsigned int>(utxo_element.second.nHeight) > state->height) {
                LogPrintf("log-compaction: %s,%s,%d: CRITICAL: UNEXPECTED ERROR OF INCONSISTENT nHeight WHILE CREATING STATE (utxo entry %u; state height: %u, utxo height: %u)!\n", __FILE__, __func__, __LINE__, debug_ctr, state->height, static_cast<unsigned int>(utxo_element.second.nHeight));
                continue;
            }
            debug_ctr++;

            // Serialize each utxo entry from the get-go so that we know its size
            // (and can respect MAX_CHUNK_SIZE).
            // Still buffer the serialized string so that we avoid performing many
            // small disk I/O operations.
            utxo_buffer.push_back(utxo_element);

            // Flush utxo buffer if we would overflow our maximum size for a single chunk
            if (overhead_size + ::GetSerializeSize(utxo_buffer, SER_DISK, CLIENT_VERSION) > MAX_CHUNK_SIZE) {
                utxo_buffer.pop_back();
                offset = CompactionState::serializeChunkFile(utxo_buffer, state, offset);
                utxo_buffer.clear();
                utxo_buffer.push_back(utxo_element);
            }
            
        }
        // By design, a non-empty UTXO set will guarantee us that something is left here
        // to flush.
        offset = CompactionState::serializeChunkFile(utxo_buffer, state, offset);
        utxo_buffer.clear();

    } catch (const std::exception& e) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I/O error while writing chunk #%u: %s\n", __FILE__, __func__, __LINE__, offset, e.what());
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Wrote all chunk files for current state.\n", __FILE__, __func__, __LINE__);

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Writing out state file.\n", __FILE__, __func__, __LINE__);

    try {
        // open new state file, named after the max block height
        CAutoFile state_file(fsbridge::fopen(state->fileName, "wb"), SER_DISK, CLIENT_VERSION);

        // write state height to file
        state_file << state->height;

        // write hash of last contained block to file
        state_file << state->latestBlockHash;
        // write number of chunks to file
        state->numChunks = offset;
        state_file << state->numChunks;

        // close the state file
        state_file.fclose();

    } catch (const std::exception& e) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I/O error while writing state file %s: %s\n", __FILE__, __func__, __LINE__, state->fileName, e.what());
    }

}

void CompactionState::setWantToCreateState(const unsigned int height, std::unique_ptr<CompactionState>* state_target, std::unique_ptr<CompactionState>* state_previous, bool will_create_dummy_state) {
    CompactionState::want_to_create_state = true;
    CompactionState::desired_state_height = height;
    CompactionState::desired_state_target = state_target;
    CompactionState::desired_state_previous = state_previous;
    CompactionState::will_create_dummy_state = will_create_dummy_state;
}

bool CompactionState::checkWantToCreateState(void) {
    return CompactionState::want_to_create_state;
}

void CompactionState::setNoMoreBlocksInFlight(bool v) {
    CompactionState::blocks_in_flight = !v;
}

void CompactionState::haltSending(CNode* node) {
    node->fHaltSend = true;
}

void CompactionState::haltReceiving(CNode* node) {
    node->fHaltRecv = true;
}

void CompactionState::doneReceiving(CNode* node) {
    node->fHaltRecvEffective = node->fHaltRecv ? true : false;
}

bool CompactionState::tryHaltReceiving() {
    if (CompactionState::communication_paused && !CompactionState::blocks_in_flight) {
        g_connman->ForEachNode(CompactionState::doneReceiving);
    }
}

void CompactionState::resumeCommunication(CNode* node) {
    node->fHaltSend = false;
    node->fHaltRecv = false;
    node->fHaltRecvEffective = false;
    // Basically tell the original code parts that not communicating during state creation was OK
    node->nLastSend = GetSystemTimeInSeconds();
    node->nLastRecv = GetSystemTimeInSeconds();
}

void CompactionState::checkCanCreateStateNode(CNode* node) {
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: STATE: Checking communication halt status for node %u: haltSend=%u, haltRecv=%u, haltRecvEffective=%u.\n", __FILE__, __func__, __LINE__, node->GetId(), node->fHaltSend, node->fHaltRecv, node->fHaltRecvEffective);
    CompactionState::can_create_state = CompactionState::can_create_state && node->fHaltSend && node->fHaltRecv && node->fHaltRecvEffective;
}

bool CompactionState::checkCanCreateState(void) {
    CompactionState::can_create_state = true;
    g_connman->ForEachNode(CompactionState::checkCanCreateStateNode);
    return CompactionState::can_create_state;
}

void CompactionState::createStateDelayed(void) {
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: STATE: Entering delayed state creation for height %u.\n", __FILE__, __func__, __LINE__, CompactionState::desired_state_height);
    if (!(CompactionState::want_to_create_state && CompactionState::can_create_state)) {
        LogPrintf("log-compaction: %s,%s,%d: STATE: Wrong want/can create flags. SHOULD NOT HAPPEN.\n", __FILE__, __func__, __LINE__);
        return;
    }

    pcoinsTip->Flush();
#  ifdef ENABLE_EVALUATION
    if (evalp_state_creation != nullptr && evalp_state_creation->isActive()) {
        evalp_state_creation->startTimerAll();
    }
#  endif
    if (CompactionState::desired_state_target != nullptr) {
        (*CompactionState::desired_state_target).reset();
    }
    (*CompactionState::desired_state_target) = std::move(CompactionState::createState(CompactionState::desired_state_height));
#  ifdef ENABLE_EVALUATION
    if (evalp_state_creation != nullptr && evalp_state_creation->isActive()) {
        evalp_state_creation->stopTimerAll();
        evalp_state_creation->doMeasurement(CompactionState::desired_state_height);
    }
#endif
    if (CompactionState::will_create_dummy_state) {
        (*CompactionState::desired_state_target).reset();
    } else {
        if (CompactionState::desired_state_previous != nullptr) {
            (*CompactionState::desired_state_target)->setPrevious(*CompactionState::desired_state_previous);
        }
    }

    CompactionState::want_to_create_state = false;
    CompactionState::desired_state_height = 0;
    CompactionState::desired_state_target = nullptr;
    CompactionState::desired_state_previous = nullptr;
    CompactionState::will_create_dummy_state = false;
    CompactionState::communication_paused = false;
    CompactionState::blocks_in_flight = true; // At least, it's potentially true unless we explicitly set it to not be
    
    g_connman->ForEachNode(CompactionState::resumeCommunication);
}


std::unique_ptr<CompactionState> CompactionState::createState(const unsigned int height) {
    // create new state object
    std::unique_ptr<CompactionState> state(new CompactionState());

    // Create compactable view on current blockchain tip
    std::unique_ptr<CCoinsViewCompaction> pcoins(new CCoinsViewCompaction(pcoinsTip.get()));
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: UTXO size before rewinding: %u.\n", __FILE__, __func__, __LINE__, pcoins->GetSize());

    // rewind UTXO database to given height
    rewindUTXOToAndForget(height, pcoins);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: UTXO size after rewinding: %u.\n", __FILE__, __func__, __LINE__, pcoins->GetSize());

#ifdef ENABLE_EVALUATION
    if (evalp_state_creation != nullptr && evalp_state_creation->isActive()) {
        evalp_state_creation->startTimerCreateOnly();
    }
#endif
    { // to unlock cs_main after accessing chainActive
        LOCK(cs_main);
        unsigned int latestBlockHeight = chainActive.Height();

        // set height for this state either to given parameter or height of latest block
        state->height = (height == 0 || height > latestBlockHeight) ? latestBlockHeight : height;

        // retrieve the hash of block at state->height
        state->latestBlockHash = chainActive[state->height]->GetBlockHash();
    }

    // create file name
    state->fileName = state->createFileName();

    serializeStateFile(state, pcoins);
#ifdef ENABLE_EVALUATION
    if (evalp_state_creation != nullptr && evalp_state_creation->isActive()) {
        evalp_state_creation->stopTimerCreateOnly();
    }
#endif

    pcoins.reset();

    // calculate hash value
    state->stateFileHash = calculateHashFromFile(state->fileName);
    state->stateHash = state->calculateStateHash();

    // write to log
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Determined saved state ID: %s.\n", __FILE__, __func__, __LINE__, (state->stateHash).ToString());
    LogPrintf("log-compaction: %s,%s,%d: Created state at height %d with hash %s\n", __FILE__, __func__, __LINE__, state->getHeight(), state->getHash().ToString());

    std::unique_ptr<CCoinsViewCompaction> pcoins_test(new CCoinsViewCompaction(pcoinsTip.get()));
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Control UTXO size after state creation: %u.\n", __FILE__, __func__, __LINE__, pcoins_test->GetSize());
    pcoins_test.reset();

    return state;
}

// Iteratively create multiple states by rewinding (and redoing) exactly once
// If from_end is true, decrease by step_size blocks from the current tip, otherwise create states on multiples of step_size (as if looking from the front)
// Setting number_states to any non-zero value will create up to this number of states and then resume.
// NOTE: This will only unwind the blocks (and forget about them!), create states accordingly, and then SHUTDOWN (for memory reasons).
// NOTE: To be extra safe, make a copy of your LevelDB database of the UTXO set
void CompactionState::createEvalStates(const unsigned int step_size, const unsigned int number_states, const bool from_end) {

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I'm going to create MULTIPLE states, starting from the %s.\n", __FILE__, __func__, __LINE__, ((from_end) ? "end" : "front"));

    if (number_states == 0) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I create as many states as possible with step size %d.\n", __FILE__, __func__, __LINE__, step_size);
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I create up to %d states with step size %d.\n", __FILE__, __func__, __LINE__, number_states, step_size);
    }

    unsigned int latestBlockHeight = 0;
    { // to unlock cs_main after accessing chainActive
        LOCK(cs_main);
        latestBlockHeight = chainActive.Height();
    }

    // count number of states created to abort after target number (if given)
    unsigned int number_states_created = 0;

    // if we simulate creating states from the front (while still going
    // backwards), we need to respect the step size. Hence, cut off the
    // remainder blocks so going back ends up in the genesis block.
    unsigned int target_state_height = latestBlockHeight;
    if (!from_end) {
        if (number_states == 0) {
            target_state_height -= (latestBlockHeight % step_size);
        } else {
            target_state_height = number_states * step_size;
        }
    }

    std::unique_ptr<CompactionState> state;
    std::unique_ptr<CCoinsViewCompaction> pcoins(new CCoinsViewCompaction(pcoinsTip.get()));

    while ((number_states == 0 || number_states_created < number_states) && target_state_height > 0) {
        
        // create new state object
        state.reset(new CompactionState());

        // set state height properly
        state->height = target_state_height;

        { // to unlock cs_main after accessing chainActive
            // retrieve the hash of block at state->height
            LOCK(cs_main);
            state->latestBlockHash = chainActive[state->height]->GetBlockHash();
        }

        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Creating state %d/%d at height %d (block hash there is %s).\n", __FILE__, __func__, __LINE__, (number_states_created + 1), number_states, state->height, state->latestBlockHash.ToString());

        // create file name
        state->fileName = state->createFileName();

        // rewind UTXO database to given height
        rewindUTXOToAndForget(state->height, pcoins);

        serializeStateFile(state, pcoins);

        // calculate hash value
        state->stateFileHash = calculateHashFromFile(state->fileName);
        state->stateHash = state->calculateStateHash();

        // write to log
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Determined saved state ID: %s.\n", __FILE__, __func__, __LINE__, (state->stateHash).ToString());
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Created state at height %d with hash %s\n", __FILE__, __func__, __LINE__, state->getHeight(), state->getHash().ToString());

        number_states_created++;
        target_state_height -= step_size;
    }

    pcoins.reset();

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Done creating states.\n", __FILE__, __func__, __LINE__);

    return;
}



std::unique_ptr<CompactionState> CompactionState::loadState(const std::string& fileName) {
    // create new state object
    std::unique_ptr<CompactionState> state(new CompactionState());
    state->fileName = GetStateDir() + fileName;

    // open the file
    CAutoFile state_file(fsbridge::fopen(state->fileName, "rb"), SER_DISK, CLIENT_VERSION);

    // check whether file exists
    if (state_file.IsNull()) {
        throw std::runtime_error("File '" + state->fileName + "' could not be found!");
    }


    try {
        // extract state height
        state_file >> state->height;

        // extract latest block hash
        state_file >> state->latestBlockHash;

        // extract number of chunks
        state_file >> state->numChunks;

        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Height of loaded state: %d.\n", __FILE__, __func__, __LINE__, state->height);
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Latest block hash: %s.\n", __FILE__, __func__, __LINE__, state->latestBlockHash.ToString());
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Number of chunks in state: %d.\n", __FILE__, __func__, __LINE__, state->numChunks);

        // close the file
        state_file.fclose();
    } catch (const std::exception& e) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I/O error while loading state: %s\n", __FILE__, __func__, __LINE__, e.what());
    }

    std::unique_ptr<CompactionChunk> chunk;
    for (unsigned i = 0; i < state->numChunks; i++) {

        chunk = std::move(loadChunk(createChunkFileName(state->height, i)));

        state->mapHashToChunk[chunk->chunkHash] = chunk->offset;
        state->chunks.push_back(std::move(chunk));
    }

    // calculate hash value
    state->stateFileHash = calculateHashFromFile(state->fileName);
    state->stateHash = state->calculateStateHash();

    // write to log
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Loaded state file %s at height %d with hash %s\n", __FILE__, __func__, __LINE__,state->getFileName(), state->getHeight(), state->getHash().ToString());
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Whole chunk database looks as follows:\n", __FILE__, __func__, __LINE__);

    // return newly created state
    return state;
}


void CompactionState::loadToChainStateDatabase() {

    try {
        LOCK(cs_main);
        in_state_loading_phase = true;

        // Used to really apply the new changes to the chainstate
        auto goodbyeview = pcoinsdbview.release();
        delete goodbyeview;
        auto goodbyecache = pcoinsTip.release();
        delete goodbyecache;
        pcoinsdbview.reset(new CCoinsViewDB(compaction_coindbcache, false, true));
        pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));


        // open the file
        CAutoFile state_file(fsbridge::fopen(fileName, "rb"), SER_DISK, CLIENT_VERSION);

        // extract state height
        state_file >> this->height;

        // extract latest block hash
        state_file >> this->latestBlockHash;
        pcoinsTip->SetBestBlock(latestBlockHash);  // Here, we require that pcoinsTip is used (actually want to change the state!)

        // extract number of chunks
        state_file >> numChunks;

        // close the file
        state_file.fclose();
    } catch (const std::exception& e) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I/O error while loading state file: %s\n", __FILE__, __func__, __LINE__, e.what());
        in_state_loading_phase = false;
        return;
    }

    // Apply UTXOs from state
    unsigned long ctr_total = 0;
    for (size_t i = 0; i < chunks.size(); i++) {

        try {
            LOCK(cs_main);

            CAutoFile chunk_file(fsbridge::fopen(this->chunks.at(i)->fileName, "rb"), SER_DISK, CLIENT_VERSION);

            // extract state height
            chunk_file >> this->chunks.at(i)->height;

            // extract offset
            chunk_file >> this->chunks.at(i)->offset;

            LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Loading chunk file with data:\n    Height: %d\n    Offset: %d\n    UTXOs: %d\n", __FILE__, __func__, __LINE__, this->chunks.at(i)->height, this->chunks.at(i)->offset, this->chunks.at(i)->n_utxos);

            std::vector<std::pair<COutPoint, Coin>> chunk_utxos;
            Unserialize(chunk_file, chunk_utxos);
            this->chunks.at(i)->n_utxos = chunk_utxos.size();


            unsigned long ctr = 0;
            for (std::pair<COutPoint, Coin> chunk_utxo : chunk_utxos) {
                // Add coin to UTXO set; overwrite is false to be safe
                pcoinsTip->AddCoin(std::move(chunk_utxo.first), std::move(chunk_utxo.second), false);

                ctr++;
            }
            ctr_total += ctr;

            chunk_file.fclose();

            // allow interruption, because the loop can take some time
            boost::this_thread::interruption_point();
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Applied chunk %d / %d (%u UTXOs)\n", __FILE__, __func__, __LINE__, i + 1, chunks.size(), ctr);

        } catch (const std::exception& e) {
            LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: I/O error while loading chunk #%u: %s\n", __FILE__, __func__, __LINE__, i + 1, e.what());
            in_state_loading_phase = false;
            return;
        }

    }

    {
        LOCK(cs_main);
        // write remaining transactions to database files
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Flushing new coins cache for state %s to UTXO database.\n", __FILE__, __func__, __LINE__, fileName);
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Number of cache elements: %u.\n", __FILE__, __func__, __LINE__, pcoinsTip->GetCacheSize());

        pcoinsTip->Flush();
        BlockMap::iterator it = mapBlockIndex.find(this->latestBlockHash);
        assert(it != mapBlockIndex.end());
        chainActive.SetTip(it->second);

        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Applied all chunks (total: %u UTXOs)\n", __FILE__, __func__, __LINE__, ctr_total);

        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Applied state file %s to UTXO database\n", __FILE__, __func__, __LINE__, fileName);
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: chainActive Tip now: %s\n", __FILE__, __func__, __LINE__, chainActive.Tip()->phashBlock->ToString());

        in_state_loading_phase = false;
    }
}

CompactionState::CompactionState() {
    confirmed = false;
    height = 0;
    prevState = nullptr;
}

unsigned int CompactionState::getHeight() const {
    return height;
}

uint256 CompactionState::getHash() const {
    return stateHash;
}

uint256 CompactionState::getLatestBlockHash() const {
    return latestBlockHash;
}

std::string CompactionState::getFileName() const {
    return fileName;
}

const CompactionState* CompactionState::getPrevious() const {
    return prevState.get();
}

bool CompactionState::isConfirmed() const {
    return confirmed;
}

std::unique_ptr<CompactionChunk> CompactionState::loadChunk(const std::string& fileName) {

    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Loading state chunk from file %s.\n", __FILE__, __func__, __LINE__, fileName);

    // check whether file exists
    CAutoFile chunk_file(fsbridge::fopen(fileName, "rb"), SER_DISK, CLIENT_VERSION);
    if (chunk_file.IsNull()) {
        throw std::runtime_error("File '" + fileName + "' could not be found!");
    }

    std::unique_ptr<CompactionChunk> chunk(new CompactionChunk());
    chunk->fileName = fileName;

    try {
        // extract state height
        chunk_file >> chunk->height;

        // extract offset
        chunk_file >> chunk->offset;

        // extract number of transactions in chunk
        std::vector<std::pair<COutPoint, Coin>> chunk_utxos;
        Unserialize(chunk_file, chunk_utxos);
        chunk->n_utxos = chunk_utxos.size();

        chunk_file.fclose();
    } catch (const std::exception& e) {
        LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: I/O error while loading chunk %s: %s\n", __FILE__, __func__, __LINE__, fileName, e.what());
    }

    chunk->chunkHash = calculateHashFromFile(fileName);

    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Successfully loaded state chunk.\n", __FILE__, __func__, __LINE__);
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Chunk hash: %s\n", __FILE__, __func__, __LINE__, chunk->chunkHash.ToString());
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: State height of chunk: %d\n", __FILE__, __func__, __LINE__, chunk->height);
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Offset of chunk in state: %d\n", __FILE__, __func__, __LINE__, chunk->offset);
    LogPrint(BCLog::COMPACTION_DETAIL, "log-compaction: %s,%s,%d: Chunk contains UTXOs: %d\n", __FILE__, __func__, __LINE__, chunk->n_utxos);

    return chunk;
}

const std::string GetStateDir() {
    if (gArgs.IsArgSet("--eval_states")) {
        return GetDataDir().string() + "/compaction_states_eval";
    } else if (gArgs.IsArgSet("--mockup_states")) {
        return GetDataDir().string() + "/compaction_states_mockup";
    } else {
        return GetDataDir().string() + "/compaction_states/";
    }
}

uint256 CompactionState::getFileHash() const {
    return this->stateFileHash;
}

const std::vector<std::unique_ptr<CompactionChunk>>* CompactionState::getChunks() const {
    return &(this->chunks);
}

std::map<uint256, int> CompactionState::getMapHashToChunk() const {
    return this->mapHashToChunk;
}

bool CompactionState::isHashValid(uint256 hash) const {
    return (stateHash == hash || stateFileHash == hash || !(mapHashToChunk.find(hash) == mapHashToChunk.end()));
}

void CompactionState::setFileName(const std::string& fileName) {
    this->fileName = fileName;
}

void CompactionState::setPrevious(std::unique_ptr<CompactionState> &previous) {
    this->prevState = std::move(previous);
}

void CompactionState::resetPrevious() {
    this->prevState.reset();
}

void CompactionState::setConfirmed() {
    this->confirmed = true;
}

std::string CompactionState::createFileName() {
    return CompactionState::createFileName(this->height);
}

std::string CompactionState::createFileName(unsigned int height) {
    std::stringstream state_name;
    state_name << std::setfill('0') << std::setw(10) << height;
    return std::string(GetStateDir() + state_name.str() + ".state");
}

std::string createChunkFileName(std::string state_filename, unsigned int offset) {
    std::stringstream s;
    s << std::setfill('0') << std::setw(4) << offset;
    return GetStateDir() + "chunks/" + state_filename + "_" + s.str() + ".chunk";
}

std::string createChunkFileName(unsigned int block_height, unsigned int offset) {
    std::stringstream state_filename;
    state_filename << std::setfill('0') << std::setw(10) << block_height;
    return createChunkFileName(state_filename.str(), offset);
}

bool IsStateCurrentlyLoading() {
    return in_state_loading_phase;
}

#endif
#endif
