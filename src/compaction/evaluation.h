#include "params.h"
#ifdef COMSYS_COMPACTION

#include "compaction.h"

#ifdef ENABLE_COMPACTION

#ifndef EVALUATION_H_
#define EVALUATION_H_

#ifdef ENABLE_EVALUATION

/* Everything in milliseconds */
class Timer {
    private:
        int64_t startTime = 0;
        int64_t stopTime = 0;
        int64_t cumulativeTime = 0;
    public:
        void start() {
            this->startTime = GetTimeMillis();
        }
        void stop() {
            this->stopTime = GetTimeMillis();
            this->cumulativeTime += (this->stopTime - this->startTime);
        }
        void reset() {
            this->startTime = 0;
            this->stopTime = 0;
        }
        bool running() {
            return this->startTime != 0;
        }
        int64_t getDuration() {
            return (this->stopTime - this->startTime);
        }
        int64_t getCumulativeDuration() {
            return this->cumulativeTime;
        }
        int64_t getStarttime() {
            return this->startTime;
        }
};

enum EvalTimerNames {
    EVAL_TIMER_SYNC_TOTAL = 0,
    EVAL_TIMER_PREPARATION_TOTAL,
    EVAL_TIMER_FULL_SYNC_TOTAL,
    EVAL_TIMER_BLOCK_TOTAL,
    EVAL_TIMER_CHECK_BLOCK,
    EVAL_TIMER_STORE_BLOCK,
    EVAL_TIMER_CONNECT_BLOCK,
};

extern const std::vector<EvalTimerNames> all_eval_timers;

class EvalPerspective {
    protected:
        std::string method_name;
        std::fstream output_file;
        void writeData(std::string data);
        bool active;
        void close();
    public:
        EvalPerspective(std::string method_name) : method_name(method_name), active(false)
        {}
        virtual ~EvalPerspective() {
            this->close();
        }
        bool createFile();
        const std::string getMethodName();
        bool isActive();
        bool base_activate() {
            this->active = true;
            return this->createFile();
        }
        /** Write header line to output file */
        virtual void writeHeader() {}
};

class EvalVanillaSynchronization : public EvalPerspective {
    /* Full synchronization by downloading the whole blockchain */
    private:
        unsigned int n_run;
        unsigned int n_state_height;
        unsigned int n_tail_length;
        unsigned int n_block_count;
        unsigned int n_block_height_max;
        bool in_tail_phase;
        std::vector<Timer> timers;

        unsigned int tail_lengths[5] = {10, 100, 144, 1000, 1008};

        void writeHeader();
    public:
        EvalVanillaSynchronization(unsigned int n_run, unsigned int n_state_height, unsigned int n_tail_length) :
                EvalPerspective("fullSync"),
                n_run(n_run),
                n_state_height(n_state_height),
                n_tail_length(n_tail_length),
                n_block_count(0),
                n_block_height_max(n_state_height + n_tail_length),
                in_tail_phase(false)
        {
            for (unsigned int i = 0; i < all_eval_timers.size(); ++i) {
                this->timers.push_back(Timer());
            }
        }
        void activate() {
            if (!this->base_activate())
                this->writeHeader();
        }
        void startMeasurement(EvalTimerNames timer_name);
        void stopMeasurement(EvalTimerNames timer_name);
        void incrementBlockcount();
        unsigned int getBlockcount();
        void doMeasurement(unsigned int block_height);
        bool synchronizationFinished();
        void enterTailPhase();
        bool inTailPhase();
        unsigned int getTailLength();
        unsigned int getTargetChainLength();
        bool onRelevantTailHeight();
};

class EvalCompactionSynchronization : public EvalPerspective {
    /* Efficient (full) synchronization by using a compaction state */
    private:
        unsigned int n_run;
        unsigned int n_state_height;
        unsigned int n_tail_length;
        unsigned int n_block_count;
        unsigned int n_block_height_max;
        bool in_tail_phase;
        std::vector<Timer> timers;

        void writeHeader();
    public:
        EvalCompactionSynchronization(unsigned int n_run, unsigned int n_state_height, unsigned int n_tail_length) :
                EvalPerspective("compactionSync"),
                n_run(n_run),
                n_state_height(n_state_height),
                n_tail_length(n_tail_length),
                n_block_count(0),
                n_block_height_max(n_state_height + n_tail_length),
                in_tail_phase(false)
        {
            for (unsigned int i = 0; i < all_eval_timers.size(); ++i) {
                this->timers.push_back(Timer());
            }
        }
        void activate() {
            if (!this->base_activate())
                this->writeHeader();
        }
        void startMeasurement(EvalTimerNames timer_name);
        void stopMeasurement(EvalTimerNames timer_name);
        void setBlockcount(unsigned int block_height);
        void incrementBlockcount();
        unsigned int getBlockcount();
        void doMeasurement(unsigned int block_height);
        bool synchronizationFinished();
        void enterTailPhase();
        bool inTailPhase();
        unsigned int getTailLength();
        unsigned int getTargetChainLength();
};

class EvalHeaderchainDownload : public EvalPerspective {
    /* Download duration of the header chain */
    private:
        unsigned int n_run;
        unsigned int n_block_height_max;
        unsigned int n_block_count;
        int64_t t_duration_ms;
        Timer timer;
        std::map<unsigned int, bool> headersReceived;

        void writeHeader();
    public:
        EvalHeaderchainDownload(unsigned int n_run, unsigned int block_height_max) :
                EvalPerspective("headerChainDownload"),
                n_run(n_run),
                n_block_height_max(block_height_max),
                n_block_count(0),
                t_duration_ms(0)
        {
        }
        void activate() {
            if (!this->base_activate())
                this->writeHeader();
        }
        void startMeasurement();
        void stopMeasurement();
        void doMeasurement(unsigned int block_height);
        bool headerAlreadySeen(unsigned int block_height);
        void headerReceived(unsigned int block_height);
        unsigned int getBlockcount();
        void incrementBlockcount();
        bool synchronizationFinished();
};

class EvalStateCreation : public EvalPerspective {
    /* Duration of creating a state from UTXO set */
    private:
        unsigned int n_runs;
        Timer timer_all;
        Timer timer_create;
        unsigned int stored_block_height;

        void writeHeader();
        void doMeasurement(unsigned int n_run, unsigned int block_height);
    public:
        EvalStateCreation(unsigned int n_runs) :
            EvalPerspective("createState"),
            n_runs(n_runs),
            stored_block_height(0)
        {}   
        void activate() {
            if (!this->base_activate())
                this->writeHeader();
        }
        void startTimerAll(void);
        void stopTimerAll(void);
        void startTimerCreateOnly(void);
        void stopTimerCreateOnly(void);
        void doMeasurement(unsigned int block_height);
        void doMeasurements(unsigned int block_height);
        void doMeasurement(void);
        void prepareMeasurement(unsigned int block_height);
};

class EvalSavingPotential : public EvalPerspective {
    /* Collect numbers of real blockchain data on state size vs. blockchain size (different metrics) */
    private:
        unsigned int n_run;
        unsigned int n_block_count;
        unsigned int n_block_height_max;
        size_t cumulative_block_sizes_no_witnesses;
        size_t cumulative_block_sizes;
        size_t cumulative_block_sizes_since_state_no_witnesses;
        size_t cumulative_block_sizes_since_state;
        unsigned int last_block_height;

        unsigned int tail_lengths[5] = {10, 100, 144, 1000, 1008};

        unsigned int stored_block_height;
        uint32_t stored_block_time;
        uintmax_t stored_size_blocks_path;
        uintmax_t stored_size_chainstate_path;
        uint64_t stored_size_blockchain_rpc;
        size_t stored_cumulative_block_sizes_no_witnesses;
        size_t stored_cumulative_block_sizes;
        size_t stored_cumulative_block_sizes_since_state_no_witnesses;
        size_t stored_cumulative_block_sizes_since_state;


        uintmax_t getFileSize(boost::filesystem::path path);
        uintmax_t getFolderSize(boost::filesystem::path path);
        uintmax_t getStateDiskSize(boost::filesystem::path path, unsigned int block_height);
        uintmax_t getBlocksFolderSize();
        uintmax_t getStatesAllFoldersSize();
        uintmax_t getStatesFolderSize(unsigned int block_height);
        uintmax_t getDatadirFolderSize();
        uintmax_t getDatadirFolderSizeNoStates(uintmax_t size_datadir_path, uintmax_t size_states_whole);
        uintmax_t getUtxoFolderSize();
        uint64_t getRpcBlockchainSize();
        size_t getAccumulatedBlockSize(bool witnesses);
        size_t getAccumulatedBlockSizeSinceState(bool witnesses);
        void resetAccumulatedBlockSizeSinceState(void);

        void writeHeader();

    public:
        EvalSavingPotential(unsigned int n_block_height_max) :
                EvalPerspective("savingPotential"),
                n_run(0),
                n_block_count(0),
                n_block_height_max(n_block_height_max),
                cumulative_block_sizes_no_witnesses(0),
                cumulative_block_sizes(0),
                cumulative_block_sizes_since_state_no_witnesses(0),
                cumulative_block_sizes_since_state(0),
                last_block_height(0),
                stored_block_height(0),
                stored_block_time(0)
        {}
        void activate() {
            if (!this->base_activate())
                this->writeHeader();
        }
        void prepareMeasurement(unsigned int block_height, uint32_t block_time);
        void doMeasurement(unsigned int block_height, uint32_t block_time, bool is_state_height);
        void doPreparedMeasurement();
        unsigned int getBlockcount();
        unsigned int getMaxBlockcount();
        void incrementBlockcount();
        bool onRelevantTailHeight();
        bool synchronizationFinished();
        void updateAccumulatedBlockSize(std::shared_ptr<const CBlock> block);
};

// global variables
extern std::unique_ptr<EvalHeaderchainDownload> evalp_headerchain_download;
extern std::unique_ptr<EvalVanillaSynchronization> evalp_vanilla_synchronization;
extern std::unique_ptr<EvalStateCreation> evalp_state_creation;
extern std::unique_ptr<EvalCompactionSynchronization> evalp_compaction_synchronization;
extern std::unique_ptr<EvalSavingPotential> evalp_saving_potential;

extern unsigned int eval_start_state_height;
extern unsigned int eval_last_state_height;
extern unsigned int eval_state_height;
extern unsigned int eval_tail_length;
extern unsigned int shutdownAtHeight;
extern unsigned int eval_number_outbound_peers;

extern std::unique_ptr<CompactionState> dummy_compaction_state;

bool initEvaluation();

const std::string GetEvalDir();
const std::string GetDefaultEvalDir();


#endif
#endif
#endif
#endif
