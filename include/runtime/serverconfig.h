/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/configreader.h"
#include "core/config.h"
#include "core/logmanager.h"

/*
 * Stores RocksDB tuning parameters and persistence settings
 * used by the server configuration layer.
 * These values define write behavior, compaction strategy,
 * caching, WAL handling, and safety-related options.
 */

struct RocksDBOptions
{
     /* Directory paths */

     /* Base data directory for RocksDB storage. */

     std::string DataDir;

     /* Write buffer settings */

     /* Single write buffer size in bytes. */

     size_t WriteBufferSize = 64 * 1024 * 1024; /* 64MB */

     /* Maximum number of write buffers. */

     int MaxWriteBufferNumber = 3;

     /* Minimum buffers to merge during flush. */

     int MinWriteBufferNumberToMerge = 1;

     /* Background jobs */

     /* Total background jobs for compaction/flush. */

     int MaxBackgroundJobs = 4;

     /* Maximum background flush threads. */

     int MaxBackgroundFlushes = 1;

     /* Maximum background compaction threads. */

     int MaxBackgroundCompactions = 3;

     /* Level and compaction settings */

     /* Base target file size for compaction. */

     size_t TargetFileSizeBase = 64 * 1024 * 1024; /* 64MB */

     /* Base max bytes for level 1. */

     size_t MaxBytesForLevelBase = 256 * 1024 * 1024; /* 256MB */

     /* Size multiplier per level. */

     int MaxBytesForLevelMultiplier = 10;

     /* L0 file count to start slowing writes. */

     int Level0SlowdownWritesTrigger = 20;

     /* L0 file count to stop writes. */

     int Level0StopWritesTrigger = 36;

     /* Compression */

     /* none, snappy, zlib, lz4, zstd */

     /* Compression for most levels. */

     std::string Compression = "none";

     /* Compression for bottommost level. */

     std::string BottommostCompression = "none";

     /* Block cache and bloom filter */

     /* Block cache size in bytes. */

     size_t BlockCacheSize = 256 * 1024 * 1024; /* 256MB */

     /* Bloom filter bits per key. */

     int BloomFilterBitsPerKey = 10;

     /* Toggle bloom filter usage. */

     bool EnableBloomFilter = true;

     /* WAL settings */

     /* Disable write-ahead log. */

     bool DisableWAL = false;

     /* WAL bytes per sync (0 = every write). */

     size_t WALBytesPerSync = 0; /* 0 = sync every write */

     /* Maximum total WAL size (0 = unlimited). */

     size_t MaxTotalWALSize = 0; /* 0 = unlimited */

     /* "none", "normal", "full" - controls per-write sync behavior */

     /* WAL sync mode string. */

     std::string WALSyncMode = "normal";

     /* Performance options */

     /* Enable direct reads. */

     bool UseDirectReads = false;

     /* Enable direct I/O for flush/compaction. */

     bool UseDirectIOForFlushAndCompaction = false;

     /* Maximum open files (-1 = unlimited). */

     int MaxOpenFiles = -1; /* -1 = unlimited */

     /* Compaction style */

     /* level, universal, fifo */

     /* Compaction style name. */

     std::string CompactionStyle = "level";

     /* Block options */

     /* Block size in bytes. */

     size_t BlockSize = 4 * 1024; /* 4KB */

     /* Restart interval for block keys. */

     int BlockRestartInterval = 16;

     /* Memory mapping options */

     /* Allow memory-mapped reads. */

     bool AllowMmapReads = false;

     /* Allow memory-mapped writes. */

     bool AllowMmapWrites = false;

     /* Use random access advice on open. */

     bool AdviseRandomOnOpen = true;

     /* Safety and verification */

     /* Enable paranoid checks. */

     bool ParanoidChecks = true;

     /* Verify checksums during compaction. */

     bool VerifyChecksumsInCompaction = true;

     /* Statistics */

     /* Enable statistics collection. */

     bool EnableStatistics = false;

     /* Stats dump period in seconds. */

     int StatsDumpPeriodSec = 600; /* 10 minutes */

     /* Advanced write options */

     /* Enable pipelined writes. */

     bool EnablePipelinedWrite = false;

     /* Allow unordered writes. */

     bool UnorderedWrite = false;

     /* Table cache options */

     /* Table cache shard bits. */

     int TableCacheNumShardBits = 6; /* 2^6 = 64 shards */

     /* File management */

     /* Period for obsolete file deletion in micros. */

     size_t DeleteObsoleteFilesPeriodMicros = 21600000000ULL; /* 6 hours */

     /* Returns default RocksDB options */

     /* Returns default RocksDB options. */

     static RocksDBOptions Default()
     {
          RocksDBOptions opts;
          opts.DataDir = "";
          return opts;
     }

     /* Loads RocksDB options from config reader */

     /* Loads RocksDB options from a config reader. */

     static RocksDBOptions LoadFromConfigReader(const ConfigReader& reader);
};

/* Command line arguments structure */

struct CommandLine
{
     /* Argument count. */

     int argc = 0;
     /* Argument vector. */

     char** argv = nullptr;
};

/* Bind configuration for a single port */

struct BindConfig
{
     /* Bind address. */

     std::string address = "0.0.0.0";

     /* Bind port. */

     int port = 9200;

     /* Bind type label. */

     std::string type = "clients";

     /* SSL settings */
     /* Enable SSL for this bind. */

     bool ssl = false;

     /* SSL certificate path. */

     std::string ssl_cert;

     /* SSL private key path. */

     std::string ssl_key;

     /* SSL protocol list. */

     std::string ssl_protocols;

     /* SSL cipher list. */

     std::string ssl_ciphers;
};

/* Server configuration management using XML-like config format */

class ServerConfig
{
   public:

     struct ModuleLoadEntry
     {
          std::string Name;

          std::string Path;
     };

     struct AIModelDescriptor
     {
          std::string Name;

          std::string File;

          bool IsDefault = false;
     };

     /* Constructor */

     /* Initializes configuration with optional command line args. */

     ServerConfig(int argc = 0, char** argv = nullptr);

     /* Destructor */

     /* Cleans up configuration resources. */

     ~ServerConfig();

     /* Load configuration from file */

     /* Loads configuration from config file path. */

     bool LoadConfig(const std::string& config_file = HLQUERY_CONFIG_DIR "/hlquery.conf");

     /* Check if configuration is valid */

     /* Returns whether configuration parsed successfully. */

     bool IsValid() const
     {
          return Valid;
     }

     /* Get configuration error message */

     /* Returns configuration error message. */

     const std::string& GetError() const
     {
          return ErrorMsg;
     }

     /* Server settings */

     /* Returns server name. */

     std::string GetServerName() const
     {
          return ServerName;
     }

     /* Returns server identifier. */

     std::string GetServerId() const
     {
          return ServerID;
     }

     /* Bind settings - default/primary bind */

     /* Returns primary bind address. */

     std::string GetBindAddress() const
     {
          return Binds.empty() ? "0.0.0.0" : Binds[0].address;
     }

     /* Returns primary bind port. */

     int GetBindPort() const
     {
          return Binds.empty() ? 9200 : Binds[0].port;
     }

     /* All bind configurations */

     /* Returns all bind configurations. */

     const std::vector<BindConfig>& GetBindConfigs() const
     {
          return Binds;
     }

     /* Database settings */

     /* Returns database engine name. */

     std::string GetDatabaseEngine() const
     {
          return DBEngine;
     }

     /* Returns database shard count. */

     int GetDatabaseShards() const
     {
          return DBShards;
     }

     /* Returns whether compaction is enabled. */

     bool GetDatabaseCompaction() const
     {
          return DBCompaction;
     }

     /* Returns cache size string. */

     std::string GetDatabaseCacheSize() const
     {
          return DBCacheSize;
     }

     /* Performance settings */

     /* Returns server thread count (0 = auto). */

     int GetThreadCount() const
     {
          return ThreadCount;
     }

     /* Global max threads limit */

     /* Returns global max threads limit. */

     int GetMaxThreads() const
     {
          return MaxThreads;
     }

     /* Returns request queue size. */

     int GetQueueSize() const
     {
          return QueueSize;
     }

     /* Returns max candidate hits. */

     int GetMaxCandidates() const
     {
          return MaxCandidates;
     }

     /* Returns snippet step size. */

     int GetSnippetStep() const
     {
          return SnippetStep;
     }

     /* Returns max concurrent connections. */

     int GetMaxConnections() const
     {
          return MaxConnections;
     }

     /* Thread pool settings */

     /* Search pool thread count */

     /* Returns search pool thread count. */

     int GetSearchPoolThreads() const
     {
          return SearchPoolThreads;
     }

     /* HTTP pool thread count */

     /* Returns HTTP pool thread count. */

     int GetHTTPPoolThreads() const
     {
          return HTTPPoolThreads;
     }

     /* Write pool thread count */

     /* Returns write pool thread count. */

     int GetWritePoolThreads() const
     {
          return WritePoolThreads;
     }

     /* Management pool thread count */

     /* Returns management pool thread count. */

     int GetManagementPoolThreads() const
     {
          return ManagementPoolThreads;
     }

    const std::vector<ModuleLoadEntry>& GetModuleLoads() const
    {
         return ModuleLoads;
    }

    const std::string& GetAIModelsDirectory() const
    {
         return AIModelsDirectory;
    }

    std::string GetConfigDirectory() const
    {
         std::filesystem::path ConfigPath(ConfigFile);
         std::error_code Ec;

         if (ConfigPath.empty())
         {
              return "";
         }

         if (!ConfigPath.is_absolute())
         {
              ConfigPath = std::filesystem::absolute(ConfigPath, Ec);
         }

         const auto ParentPath = ConfigPath.parent_path();

         return ParentPath.empty() ? "" : ParentPath.string();
    }

    const std::string& GetAIModelName() const
    {
         return AIModelName;
    }

    const std::string& GetAIInferenceCommand() const
    {
         return AIInferenceCommand;
    }

    const std::vector<AIModelDescriptor>& GetAIModelCatalog() const
    {
         return AIModelCatalog;
    }

    const std::string& GetAIModelPath() const
    {
         return AIModelPath;
    }

    bool GetSamEnabled() const
    {
         return SamEnabled;
    }

    const std::string& GetSamDataDirectory() const
    {
         return SamDataDirectory;
    }

    bool GetSam25DynamicQueryWeight() const
    {
         return Sam25DynamicQueryWeight;
    }

    double GetSam25ShortQueryPhraseBoost() const
    {
         return Sam25ShortQueryPhraseBoost;
    }

    double GetSam25LongQueryPhraseBoost() const
    {
         return Sam25LongQueryPhraseBoost;
    }

    double GetSam25SourcePhraseBoostTitle() const
    {
         return Sam25SourcePhraseBoostTitle;
    }

    double GetSam25SourcePhraseBoostLabelPair() const
    {
         return Sam25SourcePhraseBoostLabelPair;
    }

    double GetSam25SourcePhraseBoostLabel() const
    {
         return Sam25SourcePhraseBoostLabel;
    }

    double GetSam25SourcePhraseBoostLlm() const
    {
         return Sam25SourcePhraseBoostLlm;
    }

     int GetSamLLMMaxIdeas() const
     {
          return SamLLMMaxIdeas;
     }

     int GetSamContextMaxIdeas() const
     {
          return SamContextMaxIdeas;
     }

     bool GetSamLogContext() const
     {
          return SamLogContext;
     }

     int GetSamLLMTimeoutMs() const
     {
          return SamLLMTimeoutMs;
     }

     const std::string& GetSamLLMCreativityMode() const
     {
          return SamLLMCreativityMode;
     }

    bool GetSam25EnableIdf() const
    {
         return Sam25EnableIdf;
    }

    double GetSam25IdfFloor() const
    {
         return Sam25IdfFloor;
    }

    double GetSam25IdfCeiling() const
    {
         return Sam25IdfCeiling;
    }

    bool GetSam25EnableDocPrior() const
    {
         return Sam25EnableDocPrior;
    }

    const std::string& GetSam25DocPriorField() const
    {
         return Sam25DocPriorField;
    }

    double GetSam25DocPriorWeight() const
    {
         return Sam25DocPriorWeight;
    }

    int GetSam25OrderedSlop() const
    {
         return Sam25OrderedSlop;
    }

    int GetSam25UnorderedWindowSlop() const
    {
         return Sam25UnorderedWindowSlop;
    }

    bool GetSam25ExactPhraseRequiresStopwords() const
    {
         return Sam25ExactPhraseRequiresStopwords;
    }

    bool GetSam25ExactPhraseIgnoreOuterStopwords() const
    {
         return Sam25ExactPhraseIgnoreOuterStopwords;
    }

    bool GetSam25EnableSynonymExpansion() const
    {
         return Sam25EnableSynonymExpansion;
    }

    double GetSam25SynonymBoost() const
    {
         return Sam25SynonymBoost;
    }

    int GetSam25MaxSynonymsPerToken() const
    {
         return Sam25MaxSynonymsPerToken;
    }

    bool GetSam25EnableNoisePenalty() const
    {
         return Sam25EnableNoisePenalty;
    }

    double GetSam25NoisePenalty() const
    {
         return Sam25NoisePenalty;
    }

    double GetSam25NoisePenaltyLlmExtra() const
    {
         return Sam25NoisePenaltyLlmExtra;
    }

    double GetSam25MinCoverage() const
    {
         return Sam25MinCoverage;
    }

    double GetSam25MinOrderedBoostForPhrase() const
    {
         return Sam25MinOrderedBoostForPhrase;
    }

    double GetSam25MinFinalScore() const
    {
         return Sam25MinFinalScore;
    }

    bool GetSam25EnableSourceDocMerge() const
    {
         return Sam25EnableSourceDocMerge;
    }

    double GetSam25SourceDocWeight() const
    {
         return Sam25SourceDocWeight;
    }

    double GetSam25SourceDocTitleWeight() const
    {
         return Sam25SourceDocTitleWeight;
    }

    double GetSam25SourceDocDescriptionWeight() const
    {
         return Sam25SourceDocDescriptionWeight;
    }

    double GetSam25SourceDocLabelsWeight() const
    {
         return Sam25SourceDocLabelsWeight;
    }

    double GetSam25SourceDocContentWeight() const
    {
         return Sam25SourceDocContentWeight;
    }

    double GetSam25SourceDocMinScore() const
    {
         return Sam25SourceDocMinScore;
    }

    double GetSam25SourceDocMergeBonus() const
    {
         return Sam25SourceDocMergeBonus;
    }

    bool GetSam25DebugExplain() const
    {
         return Sam25DebugExplain;
    }

    int GetSam25DebugLogTopK() const
    {
         return Sam25DebugLogTopK;
    }

    bool GetSam25DebugIncludeComponents() const
    {
         return Sam25DebugIncludeComponents;
    }

     /* Search settings */

     /* Returns default ranking name. */

     std::string GetDefaultRanking() const
     {
          return DefaultRanking;
     }

     /* Returns search algorithm name. */

     std::string GetSearchAlgorithm() const
     {
          return SearchAlgorithm;
     }

     /* Logs search algorithm once per process. */

     void ReportSearchAlgorithmOnce();

     /* Returns whether fuzzy search is enabled. */

     bool GetFuzzyEnabled() const
     {
          return FuzzyEnabled;
     }

     /* Returns max edit distance for fuzzy matching. */

     int GetMaxEditDistance() const
     {
          return MaxEditDistance;
     }

     /* Returns highlight start marker. */

     std::string GetHighlightStart() const
     {
          return HighlightStart;
     }

     /* Returns highlight end marker. */

     std::string GetHighlightEnd() const
     {
          return HighlightEnd;
     }

     /* Ranking parameters - unified params section */

     /* BM25/BM25+ parameters */

     /* Returns BM25 k1 parameter. */

     double GetRankingK1() const
     {
          return RankingK1;
     }

     /* Returns BM25 b parameter. */

     double GetRankingB() const
     {
          return RankingB;
     }

     /* Returns BM25+ delta parameter. */

     double GetRankingDelta() const
     {
          return RankingDelta;
     }

     /* TF-IDF parameters */

     /* Returns IDF smoothing parameter. */

     double GetRankingIdfSmooth() const
     {
          return RankingIDFSmooth;
     }

     /* Returns whether ranking is normalized. */

     bool GetRankingNormalize() const
     {
          return RankingNormalize;
     }

     /* Hybrid parameters */

     /* Returns BM25 weight for hybrid ranking. */

     double GetRankingBm25Weight() const
     {
          return RankingBM25Weight;
     }

     /* Returns TF-IDF weight for hybrid ranking. */

     double GetRankingTfidfWeight() const
     {
          return RankingTFIDFWeight;
     }

     /* Returns BM25 IDF formula mode: legacy or smooth. */

     std::string GetRankingIdfMode() const
     {
          return RankingIdfMode;
     }

     /* Returns whether negative IDF values are clamped. */

     bool GetRankingIdfClampNegative() const
     {
          return RankingIdfClampNegative;
     }

     /* Returns hybrid merge method: linear or rrf. */

     std::string GetHybridMergeMethod() const
     {
          return HybridMergeMethod;
     }

     /* Returns whether lexical/vector components are normalized before fusion. */

     bool GetHybridNormalizeComponentScores() const
     {
          return HybridNormalizeComponentScores;
     }

     /* Returns hybrid normalization method. */

     std::string GetHybridNormalizationMethod() const
     {
          return HybridNormalizationMethod;
     }

     /* Returns RRF K parameter for hybrid merge. */

     int GetHybridRrfK() const
     {
          return HybridRrfK;
     }

     /* Returns whether query-aware dynamic hybrid alpha is enabled. */

     bool GetHybridDynamicAlphaEnabled() const
     {
          return HybridDynamicAlphaEnabled;
     }

     /* Returns max term count for short queries in dynamic alpha. */

     int GetHybridShortQueryTerms() const
     {
          return HybridShortQueryTerms;
     }

     /* Returns min term count for long queries in dynamic alpha. */

     int GetHybridLongQueryTerms() const
     {
          return HybridLongQueryTerms;
     }

     /* Returns alpha used for short queries when dynamic alpha is enabled. */

     double GetHybridAlphaShort() const
     {
          return HybridAlphaShort;
     }

     /* Returns alpha used for medium queries when dynamic alpha is enabled. */

     double GetHybridAlphaMedium() const
     {
          return HybridAlphaMedium;
     }

     /* Returns alpha used for long queries when dynamic alpha is enabled. */

     double GetHybridAlphaLong() const
     {
          return HybridAlphaLong;
     }

     /* Returns candidate pool size for brute-force vector scoring. */

     int GetHybridVectorCandidateLimit() const
     {
          return HybridVectorCandidateLimit;
     }

     /* Returns whether top-K rerank is enabled after hybrid merge. */

     bool GetHybridRerankEnabled() const
     {
          return HybridRerankEnabled;
     }

     /* Returns top-K size used for reranking. */

     int GetHybridRerankTopK() const
     {
          return HybridRerankTopK;
     }

     /* Returns lexical component weight used by rerank combiner. */

     double GetHybridRerankLexicalWeight() const
     {
          return HybridRerankLexicalWeight;
     }

     /* Returns vector component weight used by rerank combiner. */

     double GetHybridRerankVectorWeight() const
     {
          return HybridRerankVectorWeight;
     }

     /* Returns additive boost when a hit appears in both lexical and vector results. */

     double GetHybridRerankCoverageBoost() const
     {
          return HybridRerankCoverageBoost;
     }

     /* Returns URL token boost. */

     double GetUrlTokenBoost() const
     {
          return UrlTokenBoost;
     }

     /* Returns URL TLD weight. */

     double GetUrlTldWeight() const
     {
          return UrlTldWeight;
     }

     /* Returns title-like boost. */

     double GetTitleLikeBoost() const
     {
          return TitleLikeBoost;
     }

     /* Returns tag-like boost. */

     double GetTagLikeBoost() const
     {
          return TagLikeBoost;
     }

     /* Returns exact match boost. */

     double GetExactMatchBoost() const
     {
          return ExactMatchBoost;
     }

     /* Returns title exact match boost. */

     double GetTitleExactBoost() const
     {
          return TitleExactBoost;
     }

     /* Returns proximity boost scale. */

     double GetProximityBoostScale() const
     {
          return ProximityBoostScale;
     }

     /* Returns proximity boost max value. */

     double GetProximityBoostMax() const
     {
          return ProximityBoostMax;
     }

     /* Recency decay parameters */

     /* Returns whether recency decay is enabled. */

     bool GetRecencyDecayEnabled() const
     {
          return RecencyDecayEnabled;
     }

     /* Returns recency decay model name. */

     std::string GetRecencyDecayModel() const
     {
          return RecencyDecayModel;
     }

     /* Returns recency base weight. */

     double GetRecencyBaseWeight() const
     {
          return RecencyBaseWeight;
     }

     /* Returns recency max boost. */

     double GetRecencyMaxBoost() const
     {
          return RecencyMaxBoost;
     }

     /* Returns recency half-life in days. */

     double GetRecencyHalfLifeDays() const
     {
          return RecencyHalfLifeDays;
     }

     /* Returns recency decay rate. */

     double GetRecencyDecayRate() const
     {
          return RecencyDecayRate;
     }

     /* Returns recency sigmoid steepness. */

     double GetRecencySigmoidSteepness() const
     {
          return RecencySigmoidSteepness;
     }

     /* Returns recency sigmoid center. */

     double GetRecencySigmoidCenter() const
     {
          return RecencySigmoidCenter;
     }

     /* Returns recency power exponent. */

     double GetRecencyPowerExponent() const
     {
          return RecencyPowerExponent;
     }

     /* Returns recency Gaussian mean. */

     double GetRecencyGaussianMean() const
     {
          return RecencyGaussianMean;
     }

     /* Returns recency Gaussian standard deviation. */

     double GetRecencyGaussianStddev() const
     {
          return RecencyGaussianStddev;
     }

     /* Returns min decay factor. */

     double GetRecencyMinDecayFactor() const
     {
          return RecencyMinDecayFactor;
     }

     /* Returns whether timestamp field is used for recency. */

     bool GetRecencyUseTimestampField() const
     {
          return RecencyUseTimestampField;
     }

     /* Returns whether recency is normalized by collection. */

     bool GetRecencyNormalizeByCollection() const
     {
          return RecencyNormalizeByCollection;
     }

     /* Returns whether recency is query-dependent. */

     bool GetRecencyQueryDependent() const
     {
          return RecencyQueryDependent;
     }

     /* Durability settings */

     /* Returns whether AOF fsync is enabled. */

     bool GetAofFsyncEnabled() const
     {
          return AOFFsyncEnabled;
     }

     /* always | everysec | no */

     /* Returns AOF fsync policy string. */

     std::string GetAofFsyncPolicy() const
     {
          return AOFFsyncPolicy;
     }

     /* Returns whether RDB encryption is enabled. */

     bool GetRdbEncryptionEnabled() const
     {
          return RDBEncryptionEnabled;
     }

     /* env:file:literal */

     /* Returns RDB encryption key source string. */

     std::string GetRdbEncryptionKeySource() const
     {
          return RDBEncryptionKeySource;
     }

     /* AOF size management settings */

     /* Returns max AOF size in bytes. */

     size_t GetAofMaxSizeBytes() const
     {
          return AOFMaxSizeBytes;
     }

     /* Returns minimum AOF size to trigger rewrite. */

     size_t GetAofRewriteMinSizeBytes() const
     {
          return AOFRewriteMinSizeBytes;
     }

     /* Returns whether AOF+RDB sync is enabled. */

     bool GetAofRdbSyncEnabled() const
     {
          return AOFRDBSyncEnabled;
     }

     /* Returns AOF rewrite threshold percent. */

     int GetAofRewriteThresholdPercent() const
     {
          return AOFRewriteThresholdPercent;
     }

     /* Output buffer limits */

     /* Returns soft output buffer limit in bytes. */

     size_t GetClientOutbufSoft() const
     {
          return ClientOutbufSoft;
     }

     /* Returns hard output buffer limit in bytes. */

     size_t GetClientOutbufHard() const
     {
          return ClientOutbufHard;
     }

     /* Runtime mode settings */

     /* Returns whether test mode is enabled. */

     bool GetTestMode() const
     {
          return TestMode;
     }

     /* Returns whether no-fork mode is enabled. */

     bool GetNoForkMode() const
     {
          return NoForkMode;
     }

     /* Returns whether debug mode is enabled. */

     bool GetDebugMode() const
     {
          return DebugMode;
     }

     /* Returns whether verbose mode is enabled. */

     bool GetVerboseMode() const
     {
          return VerboseMode;
     }

     /* Enables or disables test mode. */

     void SetTestMode(bool enabled)
     {
          TestMode = enabled;
     }

     /* Enables or disables no-fork mode. */

     void SetNoForkMode(bool enabled)
     {
          NoForkMode = enabled;
     }

     /* Enables or disables debug mode. */

     void SetDebugMode(bool enabled)
     {
          DebugMode = enabled;
     }

     /* Enables or disables verbose mode. */

     void SetVerboseMode(bool enabled)
     {
          VerboseMode = enabled;
     }

     /* Logging settings - multiple log configurations */

     /* Returns log configuration list. */

     const std::vector<LogConfig>& GetLogConfigs() const
     {
          return LogConfigs;
     }

     /* Limits (collections and documents are unlimited, only MaxResults remains) */

     /* Returns max results limit. */

     int GetMaxResults() const
     {
          return MaxResults;
     }

     /* Command line access */

     /* Returns captured command line arguments. */

     const CommandLine& GetCommandLine() const
     {
          return CmdLine;
     }

     /* Config file management */

     /* Sets config file path. */

     void SetConfigFile(const std::string& config_file)
     {
          ConfigFile = config_file;
     }

     /* Returns config file path. */

     const std::string& GetConfigFile() const
     {
          return ConfigFile;
     }

     /* Returns raw config reader for advanced usage */

     /* Returns raw config reader. */

     const ConfigReader& GetConfigReader() const
     {
          return ConfigReaderValue;
     }

     /*
     * Get LSM options loaded from configuration
     * Loads from database.conf if present, otherwise uses defaults
     */

     /* Returns RocksDB options (lazy-loaded). */

     const RocksDBOptions& GetRocksDBOptions() const;

     /* IP Allow settings */

     /* Returns IP allowlist string. */

     std::string GetIPAllow() const
     {
          return IPAllow;
     }

     /* IP Deny settings */

     /* Returns IP denylist string. */

     std::string GetIPDeny() const
     {
          return IPDeny;
     }

     /* Returns whether host deny rules exist. */

     bool HasHostDeny() const
     {
          return HostDenyDefined;
     }

     /* Returns max DNS cache size. */

     size_t GetDNSCacheMaxSize() const
     {
          return DNSCacheMaxSize;
     }

     /* Search configuration tags from search.conf */

     /* Indexing settings */

     /* Returns whether wildcards are enabled. */

     bool GetIndexingEnableWildcards() const
     {
          return IndexingEnableWildcards;
     }

     /* Returns whether collection-name wildcard matching is enabled. */

     bool GetIndexingEnableCollectionWildcards() const
     {
          return IndexingEnableCollectionWildcards;
     }

     /* Returns whether prefix matching is enabled. */

     bool GetIndexingEnablePrefixMatching() const
     {
          return IndexingEnablePrefixMatching;
     }

     /* Returns max wildcard expansions. */

     int GetIndexingMaxWildcardExpansions() const
     {
          return IndexingMaxWildcardExpansions;
     }

     /* Returns whether fields are indexed separately. */

     bool GetIndexingIndexFieldsSeparately() const
     {
          return IndexingIndexFieldsSeparately;
     }

     /* Returns whether positions are stored. */

     bool GetIndexingStorePositions() const
     {
          return IndexingStorePositions;
     }

     /* Returns whether offsets are stored. */

     bool GetIndexingStoreOffsets() const
     {
          return IndexingStoreOffsets;
     }

     /* Query settings */

     /* Returns max query length. */

     int GetQuerySettingsMaxQueryLength() const
     {
          return QuerySettingsMaxQueryLength;
     }

     /* Returns max query terms. */

     int GetQuerySettingsMaxQueryTerms() const
     {
          return QuerySettingsMaxQueryTerms;
     }

     /* Returns min query length. */

     int GetQuerySettingsMinQueryLength() const
     {
          return QuerySettingsMinQueryLength;
     }

     /* Returns whether stemming is enabled. */

     bool GetQuerySettingsEnableStemming() const
     {
          return QuerySettingsEnableStemming;
     }

     /* Returns whether synonyms are enabled. */

     bool GetQuerySettingsEnableSynonyms() const
     {
          return QuerySettingsEnableSynonyms;
     }

     /* Returns whether fuzzy queries are enabled. */

     bool GetQuerySettingsEnableFuzzy() const
     {
          return QuerySettingsEnableFuzzy;
     }

     /* Returns fuzzy max distance for query settings. */

     int GetQuerySettingsFuzzyMaxDistance() const
     {
          return QuerySettingsFuzzyMaxDistance;
     }

     /* Scoring settings */

     /* Returns minimum score threshold. */

     double GetScoringMinScoreThreshold() const
     {
          return ScoringMinScoreThreshold;
     }

     /* Returns whether scores are normalized. */

     bool GetScoringNormalizeScores() const
     {
          return ScoringNormalizeScores;
     }

     /* Returns score precision digits. */

     int GetScoringScorePrecision() const
     {
          return ScoringScorePrecision;
     }

     /* Returns whether score explanation is enabled. */

     bool GetScoringEnableScoreExplanation() const
     {
          return ScoringEnableScoreExplanation;
     }

     /* Timeout settings */

     /* Returns query timeout in ms. */

     int GetTimeoutsQueryTimeoutMs() const
     {
          return TimeoutsQueryTimeoutMS;
     }

     /* Returns indexing timeout in ms. */

     int GetTimeoutsIndexingTimeoutMs() const
     {
          return TimeoutsIndexingTimeoutMS;
     }

     /* Returns max candidates for timeouts. */

     int GetTimeoutsMaxCandidates() const
     {
          return TimeoutsMaxCandidates;
     }

     /* Returns min candidates for timeouts. */

     int GetTimeoutsMinCandidates() const
     {
          return TimeoutsMinCandidates;
     }

     /* Logging settings (from search.conf, different from main log configs) */

     /* Returns whether query logging is enabled. */

     bool GetLoggingLogQueries() const
     {
          return LoggingLogQueries;
     }

     /* Returns whether slow query logging is enabled. */

     bool GetLoggingLogSlowQueries() const
     {
          return LoggingLogSlowQueries;
     }

     /* Returns slow query threshold in ms. */

     int GetLoggingSlowQueryThresholdMs() const
     {
          return LoggingSlowQueryThresholdMS;
     }

     /* Returns whether scoring details are logged. */

     bool GetLoggingLogScoringDetails() const
     {
          return LoggingLogScoringDetails;
     }

     /* Returns logging level. */

     int GetLoggingLogLevel() const
     {
          return LoggingLogLevel;
     }

     /* Search options */

     /* Returns whether total hits are tracked. */

     bool GetSearchOptionsTrackTotalHits() const
     {
          return SearchOptionsTrackTotalHits;
     }

     /* Returns whether scores are tracked. */

     bool GetSearchOptionsTrackScores() const
     {
          return SearchOptionsTrackScores;
     }

     /* Returns whether explanations are enabled. */

     bool GetSearchOptionsExplain() const
     {
          return SearchOptionsExplain;
     }

     /* Returns whether stored fields are returned. */

     bool GetSearchOptionsStoredFields() const
     {
          return SearchOptionsStoredFields;
     }

     /* Returns whether versions are returned. */

     bool GetSearchOptionsVersion() const
     {
          return SearchOptionsVersion;
     }

     /* Returns search preference string. */

     std::string GetSearchOptionsPreference() const
     {
          return SearchOptionsPreference;
     }

     /* Returns whether request cache is enabled. */

     bool GetSearchOptionsRequestCache() const
     {
          return SearchOptionsRequestCache;
     }

     /* Returns whether partial search results are allowed. */

     bool GetSearchOptionsAllowPartialSearchResults() const
     {
          return SearchOptionsAllowPartialSearchResults;
     }

     /* Returns batched reduce size for search. */

     int GetSearchOptionsBatchedReduceSize() const
     {
          return SearchOptionsBatchedReduceSize;
     }

     /* Returns whether typed keys are enabled. */

     bool GetSearchOptionsTypedKeys() const
     {
          return SearchOptionsTypedKeys;
     }

     /* Performance settings (from search.conf performance tag) */

     /* Returns whether IDF cache is enabled. */

     bool GetPerformanceIdfCache() const
     {
          return PerformanceIDFCache;
     }

     /* Returns whether doc length cache is enabled. */

     bool GetPerformanceDocLengthCache() const
     {
          return PerformanceDocLengthCache;
     }

     /* Returns whether average length cache is enabled. */

     bool GetPerformanceAvgLengthCache() const
     {
          return PerformanceAvgLengthCache;
     }

     /* Returns max performance cache size in MB. */

     int GetPerformanceMaxCacheSizeMb() const
     {
          return PerformanceMaxCacheSizeMB;
     }

     /* Returns performance cache TTL in seconds. */

     int GetPerformanceCacheTtlSeconds() const
     {
          return PerformanceCacheTTLSeconds;
     }

     /* Limits settings (from search.conf limits tag) */

     /* Returns default result limit. */

     int GetLimitsDefaultLimit() const
     {
          return LimitsDefaultLimit;
     }

     /* Returns max result limit. */

     int GetLimitsMaxLimit() const
     {
          return LimitsMaxLimit;
     }

     /* Returns min result limit. */

     int GetLimitsMinLimit() const
     {
          return LimitsMinLimit;
     }

     /* Returns default result offset. */

     int GetLimitsDefaultOffset() const
     {
          return LimitsDefaultOffset;
     }

     /* Returns max result offset. */

     int GetLimitsMaxOffset() const
     {
          return LimitsMaxOffset;
     }

     /* Pivot-Based Normalization settings */

     /* Returns whether pivot normalization is enabled. */

     bool GetPivotNormEnabled() const
     {
          return PivotNormEnabled;
     }

     /* Returns pivot normalization pivot value. */

     double GetPivotNormPivot() const
     {
          return PivotNormPivot;
     }

     /* Clustering settings */

     /* Returns whether clustering is enabled. */

     bool GetClusterEnabled() const
     {
          return ClusterEnabled;
     }

     /* Returns cluster port. */

     int GetClusterPort() const
     {
          return ClusterPort;
     }

     /* Returns list of cluster nodes. */

     std::vector<std::string> GetClusterNodes() const
     {
          std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
          return ClusterNodes;
     }

     /* Returns configured replication slave endpoints. */

     std::vector<std::string> GetSlaveNodes() const
     {
          std::lock_guard<std::mutex> Lock(ClusterNodesMutex);
          return SlaveNodes;
     }

     /* Returns whether replica shipping is enabled. */

     bool GetReplicationEnabled() const
     {
          return ReplicationEnabled;
     }

     /* Returns replication acknowledgement mode. */

     std::string GetReplicationMode() const
     {
          return ReplicationMode;
     }

     /* Returns replication timeout budget in milliseconds. */

     int GetReplicationTimeoutMS() const
     {
          return ReplicationTimeoutMS;
     }

     /* Returns whether replication should fail write operations when slave acknowledgements are missing. */

     bool GetReplicationFailOnError() const
     {
          return ReplicationFailOnError;
     }

     /* Returns whether this server instance is configured as a read-only replica. */

     bool GetReplicaModeEnabled() const
     {
          return ReplicaModeEnabled;
     }

     /* Returns whether writes are permitted when this instance is acting as a replica. */

     bool GetReplicaAllowWrites() const
     {
          return ReplicaAllowWrites;
     }

     /* Adds a cluster node endpoint at runtime (in-memory only). */

     bool AddClusterNode(const std::string& Endpoint, std::string* OutError = nullptr);

     /* Removes a cluster node endpoint at runtime (in-memory only). */

     bool RemoveClusterNode(const std::string& Endpoint, std::string* OutError = nullptr);

     /* Clears all cluster nodes at runtime (in-memory only). */

     void ClearClusterNodes();

     /* Returns whether distributed query routing is enabled. */

     bool GetDistributedSearchEnabled() const
     {
          return DistributedSearchEnabled;
     }

     /* Returns distributed query routing mode. */

     std::string GetDistributedSearchMode() const
     {
          return DistributedSearchMode;
     }

     /* Returns whether local execution is preferred as fallback. */

     bool GetDistributedSearchPreferLocal() const
     {
          return DistributedSearchPreferLocal;
     }

     /* Returns timeout used for remote fanout operations (ms). */

     int GetDistributedSearchTimeoutMS() const
     {
          return DistributedSearchTimeoutMS;
     }

     /* Returns whether peer fanout should reuse persistent HTTP connections. */

     bool GetDistributedPersistentTransport() const
     {
          return DistributedPersistentTransport;
     }

     /* Returns max requests sent over one persistent peer connection before recycle. */

     int GetDistributedTransportBurst() const
     {
          return DistributedTransportBurst;
     }

     /* Returns whether peer sockets should auto-reconnect with timer backoff. */

     bool GetDistributedAutoReconnect() const
     {
          return DistributedAutoReconnect;
     }

     /* Returns reconnect backoff timer in ms for peer sockets. */

     int GetDistributedReconnectMS() const
     {
          return DistributedReconnectMS;
     }

     /*
     * Returns configured peer auth tokens for endpoint.
     * Endpoint must be normalized host:port.
     */

     bool GetClusterPeerTokens(const std::string& Endpoint,
                               std::string* OutPrimaryToken,
                               std::string* OutSecondaryToken) const;

     /*
     * Returns configured replica auth tokens for endpoint.
     * Endpoint must be normalized host:port.
     */

     bool GetSlavePeerTokens(const std::string& Endpoint,
                             std::string* OutPrimaryToken,
                             std::string* OutSecondaryToken) const;

   private:

     /* Whether configuration is valid. */

     bool Valid;

     /* Error message from config load. */

     std::string ErrorMsg;

     /* Backing config reader instance. */

     ConfigReader ConfigReaderValue;

     /* Captured command line values. */

     CommandLine CmdLine;

     /* Current config file path. */

     std::string ConfigFile = HLQUERY_CONFIG_DIR "/hlquery.conf";

   public:

     /* Configuration values with defaults */

     /* Default server name. */

     std::string ServerName = "hlquery.local";

     /* Default server identifier. */

     std::string ServerID = "001";

     /* Bind configuration list. */

     std::vector<BindConfig> Binds;

     /* Database engine name. */

     std::string DBEngine = "lsm";

     /* Database shard count. */

     int DBShards = 24;

     /* Toggle database compaction. */

     bool DBCompaction = true;

     /* Database cache size string. */

     std::string DBCacheSize = "256mb";

     /* Thread count (0 = auto). */

     int ThreadCount = 0; /* 0 = auto */

     /* Global maximum threads limit (default: 8). */

     int MaxThreads = 8; /* Global maximum threads limit (default: 8) */

     /* Request queue size. */

     int QueueSize = 24576;

     /* Max candidate hits. */

     int MaxCandidates = 1000;

     /* Snippet step size. */

     int SnippetStep = 20;

     /* Maximum concurrent connections (default: 1000). */

     int MaxConnections = 1000; /* Maximum concurrent connections (default: 1000) */

     /* Thread pool thread counts (0 = auto-calculate from MaxThreads) */

     /* Search pool threads (default: 0 = auto). */

     int SearchPoolThreads = 0; /* Search pool threads (default: 0 = auto) */

     /* HTTP pool threads (default: 0 = auto). */

     int HTTPPoolThreads = 0; /* HTTP pool threads (default: 0 = auto) */

     /* Write pool threads (default: 0 = auto). */

     int WritePoolThreads = 0; /* Write pool threads (default: 0 = auto) */

     /* Management pool threads (default: 0 = auto). */

     int ManagementPoolThreads = 0; /* Management pool threads (default: 0 = auto) */

     /* Names of modules configured for startup loading. */

    std::vector<ModuleLoadEntry> ModuleLoads;
    std::string AIModelsDirectory = "run/models";
    std::string AIModelName;
    std::string AIModelPath;
    std::string AIInferenceCommand;
    std::vector<AIModelDescriptor> AIModelCatalog;
    bool SamEnabled = false;
    std::string SamDataDirectory = HLQUERY_SAM_DATA_DIR;
    bool Sam25DynamicQueryWeight = true;
    double Sam25ShortQueryPhraseBoost = 1.20;
    double Sam25LongQueryPhraseBoost = 0.85;
    double Sam25SourcePhraseBoostTitle = 1.25;
    double Sam25SourcePhraseBoostLabelPair = 1.15;
     double Sam25SourcePhraseBoostLabel = 1.00;
     double Sam25SourcePhraseBoostLlm = 0.85;
     int SamLLMMaxIdeas = 6;
     int SamContextMaxIdeas = 20;
     bool SamLogContext = false;
     int SamLLMTimeoutMs = 20000;
     std::string SamLLMCreativityMode = "balanced";
    bool Sam25EnableIdf = true;
    double Sam25IdfFloor = 0.10;
    double Sam25IdfCeiling = 2.50;
    bool Sam25EnableDocPrior = false;
    std::string Sam25DocPriorField = "popularity_score";
    double Sam25DocPriorWeight = 0.08;
    int Sam25OrderedSlop = 2;
    int Sam25UnorderedWindowSlop = 3;
    bool Sam25ExactPhraseRequiresStopwords = true;
    bool Sam25ExactPhraseIgnoreOuterStopwords = true;
    bool Sam25EnableSynonymExpansion = true;
    double Sam25SynonymBoost = 0.72;
    int Sam25MaxSynonymsPerToken = 4;
    bool Sam25EnableNoisePenalty = true;
    double Sam25NoisePenalty = 0.18;
    double Sam25NoisePenaltyLlmExtra = 0.10;
    double Sam25MinCoverage = 0.50;
    double Sam25MinOrderedBoostForPhrase = 0.20;
    double Sam25MinFinalScore = 0.35;
    bool Sam25EnableSourceDocMerge = true;
    double Sam25SourceDocWeight = 0.90;
    double Sam25SourceDocTitleWeight = 1.20;
    double Sam25SourceDocDescriptionWeight = 1.08;
    double Sam25SourceDocLabelsWeight = 1.10;
    double Sam25SourceDocContentWeight = 0.92;
    double Sam25SourceDocMinScore = 0.32;
    double Sam25SourceDocMergeBonus = 0.10;
    bool Sam25DebugExplain = false;
    int Sam25DebugLogTopK = 10;
    bool Sam25DebugIncludeComponents = true;

     /* Default ranking name. */

     std::string DefaultRanking = "bm25+";

     /* Search algorithm name. */

     std::string SearchAlgorithm = "bm25+";

     /* Toggle fuzzy matching. */

     bool FuzzyEnabled = true;

     /* Max edit distance for fuzzy matching. */

     int MaxEditDistance = 2;

     /* Highlight start marker. */

     std::string HighlightStart = "<mark>";

     /* Highlight end marker. */

     std::string HighlightEnd = "</mark>";

     /* Ranking parameters from unified <params> section */

     /* BM25/BM25+ parameters */

     /* BM25 k1 parameter. */

     double RankingK1 = 1.2;

     /* BM25 b parameter. */

     double RankingB = 0.75;

     /* BM25+ delta parameter. */

     double RankingDelta = 1.0;

     /* TF-IDF parameters */

     /* IDF smoothing parameter. */

     double RankingIDFSmooth = 1.0;

     /* Toggle ranking normalization. */

     bool RankingNormalize = true;

     /* BM25 IDF mode: legacy or smooth. */

     std::string RankingIdfMode = "legacy";

     /* Clamp negative BM25 IDF values to 0. */

     bool RankingIdfClampNegative = true;

     /* Hybrid parameters */

     /* Hybrid BM25 weight. */

     double RankingBM25Weight = 0.7;

     /* Hybrid TF-IDF weight. */

     double RankingTFIDFWeight = 0.3;

     /* Hybrid merge settings */

     /* Hybrid score merge method: linear or rrf. */

     std::string HybridMergeMethod = "linear";

     /* Normalize lexical/vector component scores before fusion. */

     bool HybridNormalizeComponentScores = false;

     /* Component score normalization method: minmax or zscore. */

     std::string HybridNormalizationMethod = "minmax";

     /* RRF K parameter. */

     int HybridRrfK = 60;

     /* Enable dynamic alpha using query length buckets. */

     bool HybridDynamicAlphaEnabled = false;

     /* Query length boundaries for dynamic alpha. */

     int HybridShortQueryTerms = 2;
     int HybridLongQueryTerms = 6;

     /* Alpha values for dynamic alpha buckets. */

     double HybridAlphaShort = 0.35;
     double HybridAlphaMedium = 0.50;
     double HybridAlphaLong = 0.70;

     /* Candidate pool size for brute-force vector retrieval. */

     int HybridVectorCandidateLimit = 1000;

     /* Optional post-merge rerank controls. */

     bool HybridRerankEnabled = false;
     int HybridRerankTopK = 100;
     double HybridRerankLexicalWeight = 0.55;
     double HybridRerankVectorWeight = 0.45;
     double HybridRerankCoverageBoost = 0.10;

     /* Field-aware weighting parameters */

     /* Boost for URL tokens. */

     double UrlTokenBoost = 1.3;

     /* Weight for URL TLD tokens. */

     double UrlTldWeight = 0.3;

     /* Boost for title-like fields. */

     double TitleLikeBoost = 1.6;

     /* Boost for tag-like fields. */

     double TagLikeBoost = 1.2;

     /* Query-time boosts */

     /* Boost for exact matches. */

     double ExactMatchBoost = 1.2;

     /* Boost for exact title matches. */

     double TitleExactBoost = 1.6;

     /* Scale for proximity boost. */

     double ProximityBoostScale = 1.0;

     /* Maximum proximity boost. */

     double ProximityBoostMax = 2.0;

     /* Recency decay parameters */

     /* Toggle recency decay. */

     bool RecencyDecayEnabled = true;

     /* Recency decay model name. */

     std::string RecencyDecayModel = "exponential";

     /* Base weight for recency. */

     double RecencyBaseWeight = 1.0;

     /* Maximum recency boost. */

     double RecencyMaxBoost = 2.0;

     /* Recency half-life in days. */

     double RecencyHalfLifeDays = 30.0;

     /* Recency decay rate (ln(2)/30). */

     double RecencyDecayRate = 0.0231; /* ln(2) / 30 ≈ 0.0231 */

     /* Recency sigmoid steepness. */

     double RecencySigmoidSteepness = 0.1;

     /* Recency sigmoid center in days. */

     double RecencySigmoidCenter = 30.0;

     /* Recency power exponent. */

     double RecencyPowerExponent = 1.5;

     /* Recency Gaussian mean. */

     double RecencyGaussianMean = 0.0;

     /* Recency Gaussian standard deviation. */

     double RecencyGaussianStddev = 30.0;

     /* Minimum recency decay factor. */

     double RecencyMinDecayFactor = 0.1;

     /* Use timestamp field for recency. */

     bool RecencyUseTimestampField = true;

     /* Normalize recency by collection size. */

     bool RecencyNormalizeByCollection = false;

     /* Make recency query-dependent. */

     bool RecencyQueryDependent = false;

     /* Durability and security */

     /* Toggle AOF fsync. */

     bool AOFFsyncEnabled = true;

     /* AOF fsync policy name. */

     std::string AOFFsyncPolicy = "everysec";

     /* Toggle RDB encryption. */

     bool RDBEncryptionEnabled = false;

     /* RDB encryption key source string. */

     std::string RDBEncryptionKeySource = "env:HLQUERY_RDB_KEY";

     /* AOF size management */

     /* Max AOF size in bytes. */

     size_t AOFMaxSizeBytes = 1024 * 1024 * 1024; /* 1GB default max AOF size */

     /* Minimum AOF size in bytes to trigger rewrite. */

     size_t AOFRewriteMinSizeBytes = 64 * 1024 * 1024; /* 64MB minimum to trigger rewrite */

     /* Toggle AOF+RDB sync. */

     bool AOFRDBSyncEnabled = true;

     /* AOF rewrite threshold percent. */

     int AOFRewriteThresholdPercent = 100;

     /* Soft output buffer limit in bytes. */

     size_t ClientOutbufSoft = 1024ULL * 1024ULL * 1024ULL; /* 1GB */

     /* Hard output buffer limit in bytes. */

     size_t ClientOutbufHard = 2048ULL * 1024ULL * 1024ULL; /* 2GB */

     /* Runtime mode flags */

     /* Enable test mode. */

     bool TestMode = false;

     /* Enable no-fork mode. */

     bool NoForkMode = false;

     /* Enable debug mode. */

     bool DebugMode = false;

     /* Enable verbose mode. */

     bool VerboseMode = false;

     /* Log configuration list. */

     std::vector<LogConfig> LogConfigs;

     /* Only keep MaxResults limit, collections and documents are unlimited */

     /* Maximum number of results. */

     int MaxResults = 1000;

   private:

     /* Applies derived configuration values. */

     void ApplyConfiguration();

     /* RocksDB options storage */

     /* Lazy-loaded RocksDB options storage. */

     mutable std::unique_ptr<RocksDBOptions> RocksDBOptionsValue;

     /* Atomic for thread-safe double-checked locking */

     /* Tracks whether RocksDB options were loaded. */

     mutable std::atomic<bool> RocksDBOptionsLoaded{false};

     /* Thread safety for lazy loading */

     /* Mutex guarding RocksDB options loading. */

     mutable std::mutex RocksDBOptionsMutex;

     /* Track if search algorithm message has been printed */

     /* Tracks whether algorithm message was printed. */

     bool AlgorithmMessagePrinted = false;

   public:

     /* IP Allow settings */

     /* IP allowlist string. */

     std::string IPAllow = "";
     /* IP denylist string. */

     std::string IPDeny = "";
     /* Whether host deny rules are defined. */

     bool HostDenyDefined = false;
     /* Max DNS cache size. */

     size_t DNSCacheMaxSize = DNS_CACHE_MAX_SIZE;

     /* Search configuration from search.conf */

     /* Indexing settings */

     /* Enable wildcard queries. */

     bool IndexingEnableWildcards = true;

     /* Enable wildcard matching for collection-name search. */

     bool IndexingEnableCollectionWildcards = true;

     /* Enable prefix matching. */

     bool IndexingEnablePrefixMatching = true;

     /* Max wildcard expansions. */

     int IndexingMaxWildcardExpansions = 100;

     /* Index fields separately. */

     bool IndexingIndexFieldsSeparately = true;

     /* Store term positions. */

     bool IndexingStorePositions = true;

     /* Store term offsets. */

     bool IndexingStoreOffsets = false;

     /* Query settings */

     /* Max query length. */

     int QuerySettingsMaxQueryLength = 1000;

     /* Max query terms. */

     int QuerySettingsMaxQueryTerms = 50;

     /* Min query length. */

     int QuerySettingsMinQueryLength = 1;

     /* Enable stemming. */

     bool QuerySettingsEnableStemming = true;

     /* Enable synonyms. */

     bool QuerySettingsEnableSynonyms = true;

     /* Enable fuzzy queries. */

     bool QuerySettingsEnableFuzzy = false;

     /* Max fuzzy distance. */

     int QuerySettingsFuzzyMaxDistance = 2;

     /* Scoring settings */

     /* Minimum score threshold. */

     double ScoringMinScoreThreshold = 0.0;

     /* Normalize scores. */

     bool ScoringNormalizeScores = true;

     /* Score precision digits. */

     int ScoringScorePrecision = 6;

     /* Enable score explanation. */

     bool ScoringEnableScoreExplanation = false;

     /* Timeout settings */

     /* Query timeout in ms. */

     int TimeoutsQueryTimeoutMS = 5000;

     /* Indexing timeout in ms. */

     int TimeoutsIndexingTimeoutMS = 30000;

     /* Max candidates for timeouts. */

     int TimeoutsMaxCandidates = 10000;

     /* Min candidates for timeouts. */

     int TimeoutsMinCandidates = 10;

     /* Logging settings (from search.conf) */

     /* Log queries. */

     bool LoggingLogQueries = false;

     /* Log slow queries. */

     bool LoggingLogSlowQueries = true;

     /* Slow query threshold in ms. */

     int LoggingSlowQueryThresholdMS = 1000;

     /* Log scoring details. */

     bool LoggingLogScoringDetails = false;

     /* Logging level. */

     int LoggingLogLevel = 1; /* 0=none, 1=errors, 2=warnings, 3=all */

     /* Search options */

     /* Track total hits. */

     bool SearchOptionsTrackTotalHits = true;

     /* Track scores. */

     bool SearchOptionsTrackScores = true;

     /* Enable explain output. */

     bool SearchOptionsExplain = false;

     /* Include stored fields. */

     bool SearchOptionsStoredFields = true;

     /* Include document version. */

     bool SearchOptionsVersion = false;

     /* Preference string. */

     std::string SearchOptionsPreference = "";

     /* Enable request cache. */

     bool SearchOptionsRequestCache = true;

     /* Allow partial results. */

     bool SearchOptionsAllowPartialSearchResults = true;

     /* Batched reduce size. */

     int SearchOptionsBatchedReduceSize = 512;

     /* Use typed keys. */

     bool SearchOptionsTypedKeys = false;

     /* Performance settings (from search.conf performance tag) */

     /* Enable IDF cache. */

     bool PerformanceIDFCache = true;

     /* Enable doc length cache. */

     bool PerformanceDocLengthCache = true;

     /* Enable avg length cache. */

     bool PerformanceAvgLengthCache = true;

     /* Max cache size in MB. */

     int PerformanceMaxCacheSizeMB = 512;

     /* Cache TTL in seconds. */

     int PerformanceCacheTTLSeconds = 3600;

     /* Limits settings (from search.conf limits tag) */

     /* Default limit (0 = unlimited). */

     int LimitsDefaultLimit = 0; /* 0 = unlimited */

     /* Maximum limit. */

     int LimitsMaxLimit = 1000;

     /* Minimum limit. */

     int LimitsMinLimit = 1;

     /* Default offset. */

     int LimitsDefaultOffset = 0;

     /* Maximum offset. */

     int LimitsMaxOffset = 100000;

     /* Pivot-Based Normalization settings */

     /* Enable pivot normalization. */

     bool PivotNormEnabled = false;

     /* Pivot value for normalization. */

     double PivotNormPivot = 0.25;

     /* Clustering settings */

     /* Enable clustering. */

     bool ClusterEnabled = false;

     /* Cluster port. */

     int ClusterPort = 9300;

     /* Cluster node list. */

     std::vector<std::string> ClusterNodes;

     /* Replication slave endpoint list loaded from links.conf. */

     std::vector<std::string> SlaveNodes;

     /* Optional auth token overrides per peer endpoint. */

     std::unordered_map<std::string, std::pair<std::string, std::string>> ClusterPeerTokens;

     /* Optional auth token overrides per slave endpoint. */

     std::unordered_map<std::string, std::pair<std::string, std::string>> SlavePeerTokens;

     /* Protects cluster node list. */

     mutable std::mutex ClusterNodesMutex;

     /* Distributed search settings */

     /* Enables distributed query routing foundation. */

     bool DistributedSearchEnabled = false;

     /* Routing mode: disabled, local_first, remote_only, strict_remote. */

     std::string DistributedSearchMode = "disabled";

     /* Whether local search is preferred as fallback path. */

     bool DistributedSearchPreferLocal = true;

     /* Timeout budget for remote fanout in milliseconds. */

     int DistributedSearchTimeoutMS = 1500;

     /* Reuse peer TCP connections between distributed requests. */

     bool DistributedPersistentTransport = true;

     /* Number of requests per persistent peer socket before reconnect. */

     int DistributedTransportBurst = 32;

     /* Whether failed peer sockets should reconnect automatically with timer backoff. */

     bool DistributedAutoReconnect = true;

     /* Timer backoff in milliseconds before next reconnect attempt. */

     int DistributedReconnectMS = 1500;

     /* Whether write replication to configured slaves is enabled. */

     bool ReplicationEnabled = false;

     /* Replication acknowledgement mode: async, sync_one, quorum, all. */

     std::string ReplicationMode = "sync_one";

     /* Timeout budget for one replication request in milliseconds. */

     int ReplicationTimeoutMS = 2000;

     /* Whether master writes should fail when replication cannot reach slaves. */

     bool ReplicationFailOnError = false;

     /* Whether this server is configured to act as a replica node. */

     bool ReplicaModeEnabled = false;

     /* Whether writes are permitted on this replica instance. */

     bool ReplicaAllowWrites = false;
};
