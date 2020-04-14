#include <compaction/params.h>
#ifdef COMSYS_COMPACTION
#ifdef ENABLE_COMPACTION

#ifndef COMPACTION_H_
#define COMPACTION_H_

#include <net.h>
#include <rpc/server.h>
#include <primitives/block.h>
#include <chainparams.h>
#include <chain.h>
#include <coins.h>
#include <consensus/consensus.h>

class CompactionState;
class CCoinsViewCache;
class CCoinsViewCompaction;

typedef enum {
    NOT_CONNECTED = 0,
    NOT_REQUESTED,
    REQUESTED,
    TIMEOUT
} NodeStateStatus;

const int initialStateHeight = 10000; // Used for fastest initialization, load other states later via RPC

const int requiredConfirmations = 10;

const unsigned int TX_PER_CHUNK = 100000;
const unsigned int MAX_CHUNK_SIZE = 1000000; // Use legacy block size
const int MAX_DOWNLOADS_PER_PEER = 16; // Set equal to MAX_BLOCKS_IN_TRANSIT_PER_PEER
const int REQUIRED_STATE_OFFERS = 8; // For experiments, be conservative and require all neighbors to advertise the same state

// if true, this node distributes the last confirmed state to new nodes and puts confirmations inside mined blocks
extern bool provideState;

extern unsigned int header_chain_best_known;
extern bool full_sync_mode;

// true, if we have at least one confirmed state
extern bool syncComplete;

// information on whether a node timed out on state request
extern NodeStateStatus nodeStatus[MAX_OUTBOUND_CONNECTIONS];

// the current (unconfirmed) and previous (last confirmed) state
extern std::unique_ptr<CompactionState> currentState;
extern std::unique_ptr<CompactionState> prevState;
extern std::unique_ptr<CompactionState> downloaded_state;

// maps state hash to the vector of offering nodes
extern std::map<uint256, std::vector<CNode*>> mapOfferedStates;

// maps state hash to the vector of chunk hashes
extern std::map<uint256, std::vector<uint256>> mapStateChunks;

// maps nodes to their current provided chunk
extern std::map<CNode*, std::string> mapNodeToChunk;

extern int64_t compaction_coindbcache;

typedef enum {	NEEDED = 0,
    IN_TRANSIT = 1,
    STORED = 2
} ChunkStatus;

// maps chunk hash to their ChunkStatus
extern std::map<uint256, ChunkStatus> mapChunkStatus;

// information which state is currently requested
extern uint256 requestedState;
extern std::string requestedStateFilename;

// RPC functions
UniValue createState(const JSONRPCRequest& request);
UniValue loadState(const JSONRPCRequest& request);
UniValue overloadPrevState(const JSONRPCRequest& request);
UniValue createEvalStates(const JSONRPCRequest& request);
UniValue isReadyToServe(const JSONRPCRequest& request);
void registerCompactionRPCCommands(CRPCTable &t);

// parses command line parameters and sets global variables accordingly
void initializeCompaction();

bool switchToFullSync(std::unique_ptr<CompactionState>* downloaded_state, unsigned int header_chain_height);

// loads the last confirmed state within the active chain
void initializeState();

// checks whether the block contains a confirmation and whether a new state must be created now
void handleNewBlock(std::shared_ptr<const CBlock> block, const CBlockIndex* blockIndex);

// increases the counter for the given state hash in the confirmation map
void addConfirmation(const uint256& stateHash);

// returns the height and hash of the last confirmed state in the active chain
int searchLastConfirmedState(uint256& hash);

// undoes all changes of blocks > height to the UTXO database
void rewindUTXOTo(unsigned int height, std::vector<CBlockIndex*>& undoneBlocks, std::unique_ptr<CCoinsViewCompaction>& pcoins);
void rewindUTXOToAndForget(unsigned int height, std::unique_ptr<CCoinsViewCompaction>& pcoins);

struct CompactionChunk {
    uint256 chunkHash;
    unsigned int height;
    unsigned int offset;
    unsigned int n_utxos;
    std::string fileName;
};

// represents one state and offers factory methods to create it
class CompactionState {
    protected:
        bool confirmed;
        unsigned int height;
        uint256 stateHash;
        uint256 latestBlockHash;
        std::unique_ptr<CompactionState> prevState;
        std::string fileName;

        std::vector<std::unique_ptr<CompactionChunk>> chunks;
        std::map<uint256, int> mapHashToChunk;
        unsigned int numChunks;
        uint256 stateFileHash;

        static bool want_to_create_state;
        static bool can_create_state;
        static unsigned int desired_state_height;
        static std::unique_ptr<CompactionState>* desired_state_target;
        static std::unique_ptr<CompactionState>* desired_state_previous;
        static bool will_create_dummy_state;
        static bool communication_paused;
        static bool blocks_in_flight;

        static unsigned int serializeChunkFile(std::vector<std::pair<COutPoint, Coin>>& utxo_buffer, std::unique_ptr<CompactionState>& state, unsigned long chunk_offset);
        static void serializeStateFile(std::unique_ptr<CompactionState>& state, std::unique_ptr<CCoinsViewCompaction>& pcoins);

    public:
        // Use constructor directly only to create dummy objects!
        CompactionState();
        uint256 getHash() const;
        unsigned int getHeight() const;
        std::string getFileName() const;
        uint256 getLatestBlockHash() const;
        const CompactionState* getPrevious() const;
        bool isConfirmed() const;

        // calculates the states' hash based on the state file hash and the chunk hashes
        uint256 calculateStateHash();
        static std::unique_ptr<CompactionChunk> loadChunk(const std::string& fileName);
        const std::vector<std::unique_ptr<CompactionChunk>>* getChunks() const;
        std::map<uint256, int> getMapHashToChunk() const;
        bool isHashValid(uint256 hash) const;
        uint256 getFileHash() const;

        void setFileName(const std::string& fileName);
        void setPrevious(std::unique_ptr<CompactionState> &previous);
        void resetPrevious();
        void setConfirmed();

        // applies this state file to the UTXO database
        void loadToChainStateDatabase();

        // creates a filename based on the states' height
        static std::string createFileName(unsigned int height);
        std::string createFileName();

        // signal that, once possible, we want to create a state for a given height
        static void setWantToCreateState(const unsigned int height, std::unique_ptr<CompactionState>* state_target, std::unique_ptr<CompactionState>* state_previous, bool dummy_state);

        // check whether we currently want to create a state
        static bool checkWantToCreateState(void);

        // pause sending messages for a single node
        static void haltSending(CNode* node);

        // pause receiving messages from a single node
        static void haltReceiving(CNode* node);

        // signal that the node is not currently expecting any responses (especially in-flight blocks)
        static void doneReceiving(CNode* node);

        // After signalling that we want to pause receiving new messages, effectively shut all receiving down
        // Returns true if receiving is now shut down for the moment
        static bool tryHaltReceiving();

        // resume communication for a single node
        static void resumeCommunication(CNode* node);

        // check single node for having paused communication
        static void checkCanCreateStateNode(CNode* node);

        // Acknowledge that all remaining in-flight blocks have been received
        static void setNoMoreBlocksInFlight(bool v);

        // check whether all nodes are already paused so that we can create a state
        static bool checkCanCreateState(void);

        // create state delayed at fixed position, when wanting to create a state was indicated before that
        static void createStateDelayed(void);

        // creates a state file from the current UTXO database containing transactions up to given block height
        static std::unique_ptr<CompactionState> createState(const unsigned int height);

        // create a series of states and shutdown afterwards (development only!)
        static void createEvalStates(const unsigned int step_size, const unsigned int number_states, const bool from_end);

        // loads state from file to UTXO database
        static std::unique_ptr<CompactionState> loadState(const std::string& fileName);

};


// Utility functions

// request the last confirmed state from the given node (always returns true, signature required by ForNode())
void requestStateFrom(CNode* node);

// request up to MAX_DOWNLOADS_PER_PEER chunks from a the given node
void requestStateChunksFrom(CNode* node, unsigned int number_chunks);

// check whether a node is connected by id, function is only dummy to pass to ForNode()
bool checkConnection(CNode* node);

// returns the hash of the given file
uint256 calculateHashFromFile(const std::string& file);

// returns the hash of the given string
uint256 calculateHashFromString(const std::string& input_string);

// writes the file into the given vector
void toByteVector(const std::string& fileName, std::vector<unsigned char>& v);

// returns the position of the first byte of the confirmation hash inside the given script
int searchConfirmationStart(const CScript& script);

// appends the confirmation tag and the hash of the current state to the given script
void addConfirmationToCoinbaseScript(CScript& script);

// returns true if the block contains a confirmation and writes hash of confirmed state to given parameter
bool checkForStateConfirmation(std::shared_ptr<const CBlock> block, uint256& hash);

// returns true if the given state is sufficiently confirmed in the confirmation map
bool isConfirmed(const uint256& stateHash);

// creates a filename based on a given state name and offset
std::string createChunkFileName(std::string state_filename, unsigned int offset);
// creates a filename based on the height and offset
std::string createChunkFileName(unsigned int block_height, unsigned int offset);

bool IsStateCurrentlyLoading();

// Get directory holding state information
const std::string GetStateDir();

#endif
#endif
#endif
