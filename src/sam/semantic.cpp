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

#ifdef HLQUERY_SAM_SPLIT_INCLUDE

/* Score one stored semantic profile against the normalized query plan. */

SAMSemanticCandidate ScoreSemanticProfileMatch(const SAMSemanticQuery& QueryPlan,
                                               const SAMSemanticProfile& Profile)
{
     SAMSemanticCandidate Candidate;

     if (QueryPlan.Rewrites.empty())
     {
          return Candidate;
     }

     std::vector<std::pair<std::string, std::string>> Targets;

     if (!Profile.Subject.empty())
     {
          Targets.push_back({Profile.Subject, "semantic_subject"});
     }

     for (const auto& Alias : Profile.Aliases)
     {
          Targets.push_back({Alias, "semantic_alias"});
     }

     for (const auto& Descriptor : Profile.Descriptors)
     {
          Targets.push_back({Descriptor, "semantic_descriptor"});
     }

     for (const auto& Query : Profile.Queries)
     {
          Targets.push_back({Query, "semantic_query"});
     }

     for (const auto& Rewrite : QueryPlan.Rewrites)
     {
          const SAMQueryTokenViews RewriteViews = NormalizeSAMQueryTokenViews(nullptr, "", Rewrite);

          for (const auto& Target : Targets)
          {
               const double Match = ComputeManifestSeedStrength(
                    RewriteViews,
                    SAM::TermEntry{Target.first, "semantic_profile", Target.second, 0.74, 0.74});

               if (Match > Candidate.ProfileScore)
               {
                    Candidate.ProfileScore = Match;
                    Candidate.MatchedText = Target.first;
                    Candidate.MatchedSource = Target.second;
               }
          }
     }

     Candidate.VectorScore = ComputeSemanticVectorSimilarity(QueryPlan.Vector, Profile.Vector);
     return Candidate;
}

/* Add profile-derived semantic matches to the aggregate hit map. */

void AppendSemanticProfileHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                               rocksdb::DB* Database,
                               const std::string& Collection,
                               const SAMSemanticQuery& QueryPlan,
                               size_t MaxCandidates = 256)
{
     if (!Database || QueryPlan.Rewrites.empty() || QueryPlan.Vector.empty())
     {
          return;
     }

     size_t Scored = 0;
     bool AcceptedSemantic = false;

     auto ScoreManifest = [&](const std::string& RawValue)
     {
          if (Scored >= MaxCandidates)
          {
               return;
          }

          try
          {
               const nlohmann::json Root = nlohmann::json::parse(RawValue);
               SAM::DocumentEntry Entry;

               if (!ParseManifestValue(RawValue, Entry) || !IsSAMDocumentEntryCurrent(Entry))
               {
                    return;
               }

               SAMSemanticProfile Profile;

               if (!ParseSemanticProfileJSON(Root, Profile))
               {
                    Profile = BuildSemanticProfile(Entry.Title.empty() ? Entry.DocumentID : Entry.Title,
                                                   Entry.Terms);
               }

               const SAMSemanticCandidate Match = ScoreSemanticProfileMatch(QueryPlan, Profile);

               if (Match.ProfileScore < 0.35)
               {
                    return;
               }

               const double CombinedSemantic = std::max(Match.ProfileScore, Match.VectorScore * 0.92);

               ++Scored;

               if (CombinedSemantic < 0.52)
               {
                    return;
               }

               SAM::LookupHit Hit;
               Hit.Collection = Root.value("collection", "");
               Hit.DocumentID = Root.value("id", "");
               Hit.Title = Entry.Title.empty() ? Root.value("title", "") : Entry.Title;
               Hit.MatchedTerm = Match.MatchedText.empty() ? Profile.Subject : Match.MatchedText;
               Hit.MatchedKind = "semantic";
               Hit.MatchedSource = Match.MatchedSource.empty() ? "semantic_vector" : Match.MatchedSource;
               Hit.TermOrigin = "semantic_profile";
               Hit.MatchedPath = "semantic_profile";
               Hit.MatchedScore = CombinedSemantic;
               Hit.MatchedSignal = std::max(Match.ProfileScore, Match.VectorScore);
               Hit.Breakdown.SemanticScore = Match.ProfileScore;
               Hit.Breakdown.SemanticVectorScore = Match.VectorScore;
               Hit.Breakdown.FinalScore = CombinedSemantic;

               if (IsSAM25DebugExplainEnabled())
               {
                    std::ostringstream Stream;
                    Stream << "semantic profile=" << Match.ProfileScore
                           << " vector=" << Match.VectorScore
                           << " source=" << Hit.MatchedSource;
                    Hit.Explain = Stream.str();
               }

               if (!Hit.Collection.empty() && !Hit.DocumentID.empty())
               {
                    AccumulateSAMHit(AggregatedHits, Hit);
                    AcceptedSemantic = true;
               }
          }
          catch (...)
          {
          }
     };

     std::vector<std::string> CandidateTerms;
     std::unordered_set<std::string> SeenTerms;

     auto AddCandidateTerm = [&](const std::string& Value)
     {
          const std::string Normalized = NormalizeTerm(Value);

          if (Normalized.size() < 2 || !SeenTerms.insert(Normalized).second)
          {
               return;
          }

          CandidateTerms.push_back(Normalized);
     };

     for (const auto& Rewrite : QueryPlan.Rewrites)
     {
          AddCandidateTerm(Rewrite);

          for (const auto& Token : TokenizeNormalized(Rewrite))
          {
               if (Token.size() >= 3)
               {
                    AddCandidateTerm(Token);
               }
          }
     }

     std::unordered_set<std::string> SeenDocuments;

     for (const auto& CandidateTerm : CandidateTerms)
     {
          if (Scored >= MaxCandidates)
          {
               break;
          }

          const std::string Prefix = BuildSemanticProfilePrefix(CandidateTerm, Collection);
          std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

          for (Iterator->Seek(Prefix);
               Iterator->Valid() && Iterator->key().starts_with(Prefix) && Scored < MaxCandidates;
               Iterator->Next())
          {
               try
               {
                    const nlohmann::json Payload = nlohmann::json::parse(Iterator->value().ToString());
                    const std::string HitCollection = Payload.value("collection", "");
                    const std::string HitDocumentID = Payload.value("id", "");

                    if (HitCollection.empty() || HitDocumentID.empty())
                    {
                         continue;
                    }

                    const std::string SeenKey = HitCollection + "\n" + HitDocumentID;

                    if (!SeenDocuments.insert(SeenKey).second)
                    {
                         continue;
                    }

                    std::string ManifestValue;
                    const rocksdb::Status ManifestStatus =
                         Database->Get(rocksdb::ReadOptions(),
                                       BuildDocManifestKey(HitCollection, HitDocumentID),
                                       &ManifestValue);

                    if (ManifestStatus.ok())
                    {
                         ScoreManifest(ManifestValue);
                    }
               }
               catch (...)
               {
               }
          }
     }

     if (AcceptedSemantic)
     {
          return;
     }

     Scored = 0;
     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     size_t Scanned = 0;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix) && Scanned < MaxCandidates;
          Iterator->Next(), ++Scanned)
     {
          ScoreManifest(Iterator->value().ToString());
     }
}

/* Add pattern-based matches for SAM-style like queries. */

void AppendSAMLikePatternHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                              rocksdb::DB* Database,
                              const std::string& Collection,
                              const std::string& Query)
{
     if (!Database || !QueryUsesSAMLikePattern(Query))
     {
          return;
     }

     const std::string Pattern = NormalizeSAMLikePattern(Query);

     if (Pattern.empty())
     {
          return;
     }

     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));

     for (Iterator->Seek(Prefix); Iterator->Valid() && Iterator->key().starts_with(Prefix); Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) || !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          for (const auto& Term : Entry.Terms)
          {
               const std::string NormalizedTerm = NormalizeTerm(Term.Text);

               if (NormalizedTerm.empty() || !Wildcard::Match(NormalizedTerm, Pattern))
               {
                    continue;
               }

               SAM::LookupHit Hit;
               Hit.Collection = Entry.Collection;
               Hit.DocumentID = Entry.DocumentID;
               Hit.Title = Entry.Title;
               Hit.MatchedTerm = Term.Text;
               Hit.MatchedKind = Term.Kind;
               Hit.MatchedSource = Term.Source;
               Hit.TermOrigin = Term.Source;
               Hit.MatchedPath = "sam_like";
               Hit.MatchedScore = ClampSAMScore((ClampSAMScore(Term.Score) * 0.82) +
                                                (ClampSAMScore(Term.Signal) * 0.18) +
                                                0.12);
               Hit.MatchedSignal = std::max(ClampSAMScore(Term.Signal), 0.60);
               Hit.Breakdown.TermScore = Hit.MatchedScore;
               Hit.Breakdown.FinalScore = Hit.MatchedScore;

               if (IsSAM25DebugExplainEnabled())
               {
                    std::ostringstream Stream;
                    Stream << "sam25+ like pattern=" << Pattern
                           << " term=" << NormalizedTerm
                           << " source=" << Hit.MatchedSource
                           << " score=" << Hit.MatchedScore;
                    Hit.Explain = Stream.str();
               }

               AccumulateSAMHit(AggregatedHits, Hit);
          }
     }
}

/* Classify the strongest retrieval path used by one SAM hit. */

std::string ClassifySAMMatchedPath(const SAM::LookupHit& Hit)
{
     const bool HasTermEvidence = Hit.Breakdown.TermScore > 0.0;
     const bool HasSourceEvidence = Hit.Breakdown.SourceDocScore > 0.0 || Hit.Breakdown.SourceDocBonus > 0.0;
     const bool HasSemanticEvidence = Hit.Breakdown.SemanticScore > 0.0 ||
                                      Hit.Breakdown.SemanticVectorScore > 0.0 ||
                                      Hit.Breakdown.SemanticBonus > 0.0;

     if (HasTermEvidence && HasSourceEvidence)
     {
          return "hybrid";
     }

     if (HasTermEvidence && HasSemanticEvidence)
     {
          return "semantic_hybrid";
     }

     if (HasSourceEvidence)
     {
          return "source_doc";
     }

     if (HasSemanticEvidence)
     {
          return "semantic_profile";
     }

     return "sam_term";
}

/* Reject search-idea hits that have weak support and low score. */

bool ShouldRejectWeakSearchIdeaHit(const SAM::LookupHit& Hit)
{
     if (Hit.MatchedKind != "search_idea")
     {
          return false;
     }

     const bool HasSourceDocumentSupport =
          Hit.Breakdown.SourceDocScore > 0.0 || Hit.Breakdown.SourceDocBonus > 0.0;
     if (HasSourceDocumentSupport)
     {
          return false;
     }

     if (Hit.EvidenceCount >= 3)
     {
          return false;
     }

     return Hit.Breakdown.FinalScore < 1.20;
}

bool HasStrongSourceSupport(const SAM::LookupHit& Hit)
{
     if (Hit.Breakdown.SourceDocScore > 0.0)
     {
          return true;
     }

     return Hit.Breakdown.SourceDocBonus > 0.0 &&
            (Hit.MatchedPath == "source_doc" ||
             Hit.MatchedPath == "source_doc_fallback" ||
             Hit.MatchedPath == "hybrid");
}

bool IsSemanticLikeSAMHit(const SAM::LookupHit& Hit)
{
     return Hit.MatchedKind == "search_idea" ||
            Hit.MatchedKind == "semantic" ||
            Hit.MatchedPath == "search_idea" ||
            Hit.MatchedPath == "semantic_profile" ||
            Hit.MatchedPath == "semantic_hybrid" ||
            Hit.MatchedPath == "collection_learned";
}

bool HasCorroboratedSAMHit(const std::vector<SAM::LookupHit>& Hits)
{
     for (const auto& Hit : Hits)
     {
          if (HasStrongSourceSupport(Hit) || Hit.EvidenceCount >= 3)
          {
               return true;
          }
     }

     return false;
}

bool ShouldSuppressLowConfidenceSAMResultSet(const std::vector<SAM::LookupHit>& Hits)
{
     if (Hits.empty() || HasCorroboratedSAMHit(Hits))
     {
          return false;
     }

     const SAM::LookupHit& TopHit = Hits.front();

     if (!IsSemanticLikeSAMHit(TopHit))
     {
          return false;
     }

     return TopHit.Breakdown.FinalScore < 1.30;
}

bool HasEnoughLiteralSAMCoverage(const SAM::LookupHit& Hit,
                                 const SAMQueryTokenViews& QueryViews)
{
     if (HasStrongSourceSupport(Hit))
     {
          return true;
     }

     const double ConfiguredMinCoverage = (Instance && Instance->Config
          ? Instance->Config->GetSam25MinCoverage()
          : 0.50);
     const double RequiredCoverage = GetSAM25RequiredCoverage(QueryViews, ConfiguredMinCoverage);

     auto CoverageFor = [&](const std::string& Value)
     {
          std::vector<std::string> Tokens = TokenizeNormalized(Value);

          for (auto& Token : Tokens)
          {
               Token = SingularizeToken(Token);
          }

          return ComputeSAMLiteralQueryTokenCoverage(QueryViews, Tokens);
     };

     const double BestCoverage = std::max(CoverageFor(Hit.MatchedTerm), CoverageFor(Hit.Title));

     if (QueryViews.CoreTokens.size() <= 1)
     {
          return BestCoverage >= 1.0;
     }

     return BestCoverage >= RequiredCoverage;
}

bool IsSAMIdentifierToken(const std::string& Token)
{
     bool HasAlpha = false;
     bool HasDigit = false;

     for (unsigned char C : Token)
     {
          HasAlpha = HasAlpha || std::isalpha(C);
          HasDigit = HasDigit || std::isdigit(C);
     }

     return HasAlpha && HasDigit;
}

bool HasExactSourceDocumentIdentifierTokens(const SAM::LookupHit& Hit,
                                            const SAMQueryTokenViews& QueryViews)
{
     if (!(Instance && Instance->Config) ||
         !Instance->Config->GetSam25RequireExactIdentifierTokens())
     {
          return true;
     }

     std::vector<std::string> RequiredTokens;

     for (const auto& Token : TokenizeNormalized(QueryViews.NormalizedPhrase.empty()
               ? QueryViews.NormalizedQuery
               : QueryViews.NormalizedPhrase))
     {
          if (IsSAMIdentifierToken(Token))
          {
               RequiredTokens.push_back(Token);
          }
     }

     if (RequiredTokens.empty())
     {
          return true;
     }

     if (Hit.Collection.empty() || Hit.DocumentID.empty())
     {
          return false;
     }

     const Document StorageDoc =
          HybridStorageManager::GetInstance().GetDocument(Hit.Collection, Hit.DocumentID);

     if (StorageDoc.ID.empty())
     {
          return false;
     }

     std::unordered_set<std::string> SourceTokens;

     for (const auto& Token : TokenizeNormalized(NormalizeTerm(StorageDoc.ID)))
     {
          SourceTokens.insert(Token);
     }

     for (const auto& Field : GetSAM25SourceDocumentFields(StorageDoc))
     {
          for (const auto& Token : TokenizeNormalized(NormalizeTerm(Field.second)))
          {
               SourceTokens.insert(Token);
          }
     }

     return std::all_of(RequiredTokens.begin(), RequiredTokens.end(),
                        [&](const std::string& Token)
                        {
                             return SourceTokens.find(Token) != SourceTokens.end();
                        });
}

std::vector<float> BuildHitSemanticVector(rocksdb::DB* Database,
                                          const SAM::LookupHit& Hit)
{
     if (Database && !Hit.Collection.empty() && !Hit.DocumentID.empty())
     {
          std::string ManifestValue;
          const rocksdb::Status Status =
               Database->Get(rocksdb::ReadOptions(),
                             BuildDocManifestKey(Hit.Collection, Hit.DocumentID),
                             &ManifestValue);

          if (Status.ok())
          {
               try
               {
                    const nlohmann::json Root = nlohmann::json::parse(ManifestValue);
                    SAMSemanticProfile Profile;

                    if (ParseSemanticProfileJSON(Root, Profile) && !Profile.Vector.empty())
                    {
                         return Profile.Vector;
                    }

                    SAM::DocumentEntry Entry;

                    if (ParseManifestValue(ManifestValue, Entry))
                    {
                         return BuildSemanticProfile(Entry.Title.empty() ? Entry.DocumentID : Entry.Title,
                                                     Entry.Terms).Vector;
                    }
               }
               catch (...)
               {
               }
          }
     }

     return BuildHashedSemanticVector({
          Hit.Title,
          Hit.MatchedTerm,
          Hit.MatchedKind,
          Hit.MatchedSource
     });
}

bool HasEnoughSemanticLinearAlgebraCoherence(rocksdb::DB* Database,
                                             const SAM::LookupHit& Hit,
                                             const SAMQueryTokenViews& QueryViews)
{
     if (HasStrongSourceSupport(Hit) || !IsSemanticLikeSAMHit(Hit))
     {
          return true;
     }

     const std::string QueryText = QueryViews.NormalizedPhrase.empty()
          ? QueryViews.NormalizedQuery
          : QueryViews.NormalizedPhrase;
     const std::vector<float> QueryVector = BuildHashedSemanticVector({QueryText});
     const std::vector<float> HitVector = BuildHitSemanticVector(Database, Hit);

     if (QueryVector.empty() || HitVector.empty())
     {
          return false;
     }

     const double Cosine = ComputeSemanticVectorCosine(QueryVector, HitVector);
     const double Projection = ComputeSemanticVectorProjection(QueryVector, HitVector);
     const double ResidualRatio = ComputeSemanticVectorResidualRatio(QueryVector, HitVector);
     const double VectorScore = Hit.Breakdown.SemanticVectorScore > 0.0
          ? Hit.Breakdown.SemanticVectorScore
          : ComputeSemanticVectorSimilarity(QueryVector, HitVector);
     const double LiteralCoverage = std::max(
          ComputeSAMLiteralQueryTokenCoverage(QueryViews, NormalizeSAMTokens(Hit.MatchedTerm, true)),
          ComputeSAMLiteralQueryTokenCoverage(QueryViews, NormalizeSAMTokens(Hit.Title, true)));

     if (LiteralCoverage >= 1.0)
     {
          return Cosine >= 0.18 || VectorScore >= 0.59;
     }

     if (QueryViews.CoreTokens.size() <= 1)
     {
          return Cosine >= 0.42 && Projection >= 0.18 && ResidualRatio <= 0.92;
     }

     return Cosine >= 0.34 && Projection >= 0.14 && ResidualRatio <= 0.96 && VectorScore >= 0.58;
}

void ReplaceWithGuaranteedSourceDocFallback(std::vector<SAM::LookupHit>& Hits)
{
     if (Hits.empty())
     {
          return;
     }

     std::vector<SAM::LookupHit> PreferredHits;
     PreferredHits.reserve(Hits.size());

     for (const auto& Hit : Hits)
     {
          if (Hit.MatchedPath == "source_doc" || Hit.MatchedPath == "hybrid")
          {
               PreferredHits.push_back(Hit);
          }
     }

     if (!PreferredHits.empty())
     {
          Hits.swap(PreferredHits);
          return;
     }

     const SAM::LookupHit& TopHit = Hits.front();
     SAM::LookupHit FallbackHit = TopHit;
     FallbackHit.MatchedKind = "source_doc";
     FallbackHit.MatchedSource = "source_doc_fallback";
     FallbackHit.MatchedPath = "source_doc_fallback";
     FallbackHit.TermOrigin = FallbackHit.MatchedSource;
     FallbackHit.MatchedSignal = std::max(FallbackHit.MatchedSignal, FallbackHit.Breakdown.FinalScore);
     FallbackHit.Breakdown.SourceDocScore = std::max(FallbackHit.Breakdown.SourceDocScore,
                                                     FallbackHit.Breakdown.FinalScore);
     if (FallbackHit.Breakdown.FinalScore <= 0.0)
     {
          FallbackHit.Breakdown.FinalScore = std::max(0.56, FallbackHit.MatchedScore);
     }
     FallbackHit.MatchedScore = std::max(FallbackHit.MatchedScore, FallbackHit.Breakdown.FinalScore);

     if (IsSAM25DebugExplainEnabled())
     {
          if (!FallbackHit.Explain.empty())
          {
               FallbackHit.Explain += " ";
          }

          FallbackHit.Explain += "fallback=guaranteed_source_doc";
     }

     Hits.clear();
     Hits.push_back(std::move(FallbackHit));
}

void FinalizeSAMAggregatedHits(std::vector<SAM::LookupHit>& Hits,
                               const std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                               const SAMQueryTokenViews& QueryViews,
                               rocksdb::DB* Database,
                               size_t Limit)
{
     Hits.clear();
     Hits.reserve(AggregatedHits.size());

     for (const auto& Entry : AggregatedHits)
     {
          SAM::LookupHit FinalHit = Entry.second.BestHit;
          if (!HasExactSourceDocumentIdentifierTokens(FinalHit, QueryViews))
          {
               continue;
          }
          const size_t ExtraEvidence = Entry.second.EvidenceCount > 0 ? Entry.second.EvidenceCount - 1 : 0;
          const size_t ExtraTerms = Entry.second.DistinctTerms.size() > 0 ? Entry.second.DistinctTerms.size() - 1 : 0;
          const size_t ExtraSources = Entry.second.DistinctSources.size() > 0 ? Entry.second.DistinctSources.size() - 1 : 0;
          const double EvidenceBonus = std::min(0.18,
               (0.02 * static_cast<double>(ExtraEvidence)) +
               (0.04 * static_cast<double>(ExtraTerms)) +
               (0.02 * static_cast<double>(ExtraSources)));
          const double SemanticBonus = std::min(0.24,
               (Entry.second.BestSemanticScore * 0.16) +
               (Entry.second.BestSemanticVectorScore * 0.08));
          const double DocPrior = GetSAM25DocumentPriorScore(FinalHit);
          const SAM25SourceDocMatch SourceMatch = ComputeSAM25SourceDocumentMatch(FinalHit.Collection,
               FinalHit.DocumentID, QueryViews);
          const double SourceDocWeight = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocWeight()
               : 0.90);
          const double SourceDocMergeBonus = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocMergeBonus()
               : 0.10);
          const double SourceScoreBonus = SourceMatch.Score > 0.0
               ? std::min(SourceDocMergeBonus, SourceMatch.Score * SourceDocWeight)
               : 0.0;
          FinalHit.EvidenceCount = Entry.second.EvidenceCount;
          FinalHit.Breakdown.EvidenceBonus = EvidenceBonus;
          FinalHit.Breakdown.DocPrior = DocPrior;
          FinalHit.Breakdown.SemanticScore = std::max(FinalHit.Breakdown.SemanticScore, Entry.second.BestSemanticScore);
          FinalHit.Breakdown.SemanticVectorScore = std::max(FinalHit.Breakdown.SemanticVectorScore, Entry.second.BestSemanticVectorScore);
          FinalHit.Breakdown.SourceDocScore = std::max(FinalHit.Breakdown.SourceDocScore, Entry.second.BestSourceDocScore);
          FinalHit.Breakdown.SemanticBonus = SemanticBonus;
          FinalHit.Breakdown.SourceDocBonus = SourceScoreBonus;
          FinalHit.MatchedScore += EvidenceBonus;
          FinalHit.MatchedScore += SemanticBonus;
          FinalHit.MatchedScore += Instance && Instance->Config
               ? (DocPrior * Instance->Config->GetSam25DocPriorWeight())
               : 0.0;
          FinalHit.MatchedScore += SourceScoreBonus;
          FinalHit.Breakdown.FinalScore = FinalHit.MatchedScore;
          FinalHit.MatchedPath = ClassifySAMMatchedPath(FinalHit);
          if (FinalHit.TermOrigin.empty())
          {
               FinalHit.TermOrigin = FinalHit.MatchedSource;
          }
          if (ShouldRejectWeakSearchIdeaHit(FinalHit))
          {
               continue;
          }
          if (!HasEnoughLiteralSAMCoverage(FinalHit, QueryViews))
          {
               continue;
          }
          if (!HasEnoughSemanticLinearAlgebraCoherence(Database, FinalHit, QueryViews))
          {
               continue;
          }
          if (IsSAM25DebugExplainEnabled() && !FinalHit.Explain.empty())
          {
               std::ostringstream Stream;
               Stream << FinalHit.Explain
                      << " evidence_bonus=" << EvidenceBonus
                      << " semantic_bonus=" << SemanticBonus
                      << " doc_prior=" << DocPrior
                      << " source_doc_bonus=" << SourceScoreBonus;

               if (!SourceMatch.Field.empty())
               {
                    Stream << " source_doc_field=" << SourceMatch.Field;
               }

               if (!SourceMatch.Explain.empty())
               {
                    Stream << " source_doc_explain={" << SourceMatch.Explain << "}";
               }

               Stream
                      << " final=" << FinalHit.MatchedScore;
               FinalHit.Explain = Stream.str();
          }
          Hits.push_back(std::move(FinalHit));
     }

     std::sort(Hits.begin(), Hits.end(),
               [](const SAM::LookupHit& A, const SAM::LookupHit& B)
               {
                    if (A.MatchedScore != B.MatchedScore)
                    {
                         return A.MatchedScore > B.MatchedScore;
                    }

                    if (A.MatchedSignal != B.MatchedSignal)
                    {
                         return A.MatchedSignal > B.MatchedSignal;
                    }

                    if (A.Collection != B.Collection)
                    {
                         return A.Collection < B.Collection;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Limit > 0 && Hits.size() > Limit)
     {
          Hits.resize(Limit);
     }

     if (ShouldSuppressLowConfidenceSAMResultSet(Hits))
     {
          Hits.clear();
     }
}

double ComputeSAM25TermScore(rocksdb::DB* Database,
                             const std::string& Collection,
                             const SAMQueryTokenViews& QueryViews,
                             const SAM::TermEntry& Term,
                             SAM25ScoreDebug* DebugOut = nullptr)
{
     const std::vector<std::string>& QueryTokens = QueryViews.CoreTokens;

     if (QueryTokens.empty() || Term.Text.empty())
     {
          return -1.0;
     }

     std::vector<std::string> TermTokens = TokenizeNormalized(Term.Text);

     for (auto& Token : TermTokens)
     {
          Token = SingularizeToken(Token);
     }

     if (!TokensFuzzyMatch(QueryTokens, TermTokens))
     {
          return -1.0;
     }

     size_t Matched = 0;
     size_t LiteralMatches = 0;
     size_t DistancePenalty = 0;
     size_t SynonymMatches = 0;

     for (const auto& QueryToken : QueryTokens)
     {
          SAMTokenMatchResult BestMatch;

          for (const auto& TermToken : TermTokens)
          {
               const SAMTokenMatchResult Match = MatchSAMQueryTokenToTermToken(QueryViews, QueryToken, TermToken);

               if (!Match.Matched)
               {
                    continue;
               }

               if (!BestMatch.Matched || *Match.Distance < *BestMatch.Distance ||
                   (*Match.Distance == *BestMatch.Distance && !Match.UsedSynonym && BestMatch.UsedSynonym))
               {
                    BestMatch = Match;
               }
          }

               if (BestMatch.Matched)
               {
                    Matched++;
                    LiteralMatches += BestMatch.UsedSynonym ? 0U : 1U;
                    DistancePenalty += BestMatch.Distance.value_or(0U);
                    SynonymMatches += BestMatch.UsedSynonym ? 1U : 0U;
               }
     }

     if (Matched == 0 || LiteralMatches == 0)
     {
          return -1.0;
     }

     const double Coverage = static_cast<double>(Matched) / static_cast<double>(QueryTokens.size());
     const double EditSimilarity = ClampSAMScore(1.0 - (static_cast<double>(DistancePenalty) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size() * 2))));
     const double OrderedBoost = ClampSAMScore(ComputeOrderedWindowBoost(QueryTokens, TermTokens));
     const double UnorderedBoost = ClampSAMScore(ComputeUnorderedWindowBoost(QueryTokens, TermTokens));
     const double SynonymCoverage = ClampSAMScore(static_cast<double>(SynonymMatches) /
          static_cast<double>(std::max<size_t>(1, QueryTokens.size())));
     const double BaseTermScore = ClampSAMScore(Term.Score);
     const double SignalScore = ClampSAMScore(Term.Signal);
     const double QueryPhraseWeight = GetSAM25QueryPhraseWeight(QueryViews);
     const double ConfiguredMinCoverage = (Instance && Instance->Config ? Instance->Config->GetSam25MinCoverage() : 0.50);
     const double MinCoverage = GetSAM25RequiredCoverage(QueryViews, ConfiguredMinCoverage);
     const double MinOrderedBoostForPhrase = (Instance && Instance->Config ?
          Instance->Config->GetSam25MinOrderedBoostForPhrase() : 0.20);
     const double MinFinalScore = (Instance && Instance->Config ? Instance->Config->GetSam25MinFinalScore() : 0.35);
     const double SynonymBoost = ClampSAMScore(SynonymCoverage *
          (Instance && Instance->Config ? Instance->Config->GetSam25SynonymBoost() : 0.72));
     double PhraseBoost = 0.0;

     if (Coverage < MinCoverage)
     {
          if (DebugOut)
          {
               DebugOut->Coverage = Coverage;
               DebugOut->RejectedMinCoverage = true;
               DebugOut->FinalScore = -1.0;
          }
          return -1.0;
     }

     if (!QueryViews.NormalizedPhrase.empty())
     {
          const std::string ExactPhraseQuery = BuildSAMExactPhraseCandidate(QueryViews.NormalizedPhrase);
          const std::string ExactPhraseTerm = BuildSAMExactPhraseCandidate(Term.Text);

          if (OrderedBoost >= MinOrderedBoostForPhrase &&
               !ExactPhraseQuery.empty() && ContainsNormalizedPhrase(ExactPhraseTerm, ExactPhraseQuery))
          {
               PhraseBoost = (QueryViews.Quoted ? 0.80 : 0.30) * GetSAM25PhraseSourceWeight(Term.Source);
          }
     }

     PhraseBoost = ClampSAMScore(PhraseBoost);
     const double WeightedOrderedBoost = ClampSAMScore(OrderedBoost * QueryPhraseWeight);
     const double WeightedUnorderedBoost = ClampSAMScore(UnorderedBoost * QueryPhraseWeight);
     const double WeightedPhraseBoost = ClampSAMScore(PhraseBoost * QueryPhraseWeight);

     double Score =
          (0.22 * BaseTermScore) +
          (0.12 * SignalScore) +
          (0.20 * Coverage) +
          (0.12 * EditSimilarity) +
          (0.18 * WeightedOrderedBoost) +
          (0.09 * WeightedUnorderedBoost) +
          (0.02 * SynonymBoost) +
          (0.05 * WeightedPhraseBoost);

     Score = ClampSAMScore(Score + ComputeSingleTokenLiteralBias(QueryViews, TermTokens));
     const double NoisePenalty = GetSAM25NoisePenalty(Term);
     Score = ClampSAMScore(Score - NoisePenalty);
     const double SourceWeight = GetSAMSourceWeight(Term.Source);
     const double SpecificityWeight = GetSAM25TermSpecificityWeight(Database, Collection, Term.Text);
     Score *= SourceWeight;
     Score *= SpecificityWeight;
     if (DebugOut)
     {
          DebugOut->BaseTermScore = BaseTermScore;
          DebugOut->SignalScore = SignalScore;
          DebugOut->Coverage = Coverage;
          DebugOut->EditSimilarity = EditSimilarity;
          DebugOut->OrderedBoost = WeightedOrderedBoost;
          DebugOut->UnorderedBoost = WeightedUnorderedBoost;
          DebugOut->SynonymBoost = SynonymBoost;
          DebugOut->PhraseBoost = WeightedPhraseBoost;
          DebugOut->QueryPhraseWeight = QueryPhraseWeight;
          DebugOut->NoisePenalty = NoisePenalty;
          DebugOut->SourceWeight = SourceWeight;
          DebugOut->SpecificityWeight = SpecificityWeight;
          DebugOut->FinalScore = Score;
     }
     if (Score < MinFinalScore)
     {
          if (DebugOut)
          {
               DebugOut->RejectedMinFinalScore = true;
               DebugOut->FinalScore = Score;
          }
          return -1.0;
     }
     return Score;
}

void AppendFuzzyFallbackHits(std::unordered_map<std::string, SAMAggregatedHit>& AggregatedHits,
                             rocksdb::DB* Database,
                             const std::string& Collection,
                             const std::string& Query,
                             size_t Limit)
{
     if (!Database || Limit == 0)
     {
          return;
     }

     const SAMQueryTokenViews QueryViews = NormalizeSAMQueryTokenViews(Database, Collection, Query);
     const std::vector<std::string>& QueryTokens = QueryViews.CoreTokens;

     if (QueryTokens.empty())
     {
          return;
     }

     const bool SingleTokenIntent = IsSingleTokenSAMIntent(QueryViews);
     const size_t MaxScannedDocuments = SingleTokenIntent
          ? std::max<size_t>(128, std::min<size_t>(1024, std::max<size_t>(Limit, 1) * 32))
          : std::numeric_limits<size_t>::max();
     const std::string Prefix = Collection.empty() ? "sam:doc:" : "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<SAM::LookupHit> Candidates;
     size_t ScannedDocuments = 0;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix) && ScannedDocuments < MaxScannedDocuments;
          Iterator->Next(), ++ScannedDocuments)
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) ||
              !IsSAMDocumentEntryCurrent(Entry))
          {
               continue;
          }

          if (!Collection.empty() && Entry.Collection != Collection)
          {
               continue;
          }

          const std::string DedupKey = Entry.Collection + "/" + Entry.DocumentID;

          double BestScore = -1.0;
          SAM::LookupHit BestHit;

          for (const auto& Term : Entry.Terms)
          {
               SAM25ScoreDebug Debug;
               const double FuzzyScore = ComputeSAM25TermScore(Database, Collection, QueryViews, Term, &Debug);

               if (FuzzyScore <= BestScore)
               {
                    continue;
               }

               BestScore = FuzzyScore;
               BestHit.Collection = Entry.Collection;
               BestHit.DocumentID = Entry.DocumentID;
               BestHit.Title = Entry.Title;
               BestHit.MatchedTerm = Term.Text;
               BestHit.MatchedKind = Term.Kind;
               BestHit.MatchedSource = Term.Source;
               BestHit.TermOrigin = Term.Source;
               BestHit.MatchedPath = "sam_term";
               BestHit.MatchedScore = FuzzyScore;
               BestHit.MatchedSignal = Term.Signal;
               BestHit.Breakdown.TermScore = FuzzyScore;
               BestHit.Breakdown.FinalScore = FuzzyScore;
               if (IsSAM25DebugExplainEnabled())
               {
                    BestHit.Explain = FormatSAM25Explain(Debug);
               }
          }

          const SAM25SourceDocMatch SourceMatch = ComputeSAM25SourceDocumentMatch(Entry.Collection, Entry.DocumentID, QueryViews);
          const double SourceDocWeight = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocWeight()
               : 0.90);
          const double SourceDocMergeBonus = (Instance && Instance->Config
               ? Instance->Config->GetSam25SourceDocMergeBonus()
               : 0.10);
          const double EffectiveSourceScore = SourceMatch.Score > 0.0
               ? SourceMatch.Score * SourceDocWeight
               : -1.0;

          if (EffectiveSourceScore > BestScore)
          {
               BestScore = EffectiveSourceScore;
               BestHit.Collection = Entry.Collection;
               BestHit.DocumentID = Entry.DocumentID;
               BestHit.Title = Entry.Title;
               BestHit.MatchedTerm = QueryViews.NormalizedPhrase.empty()
                    ? QueryViews.NormalizedQuery
                    : QueryViews.NormalizedPhrase;
               BestHit.MatchedKind = "source_doc";
               BestHit.MatchedSource = SourceMatch.Field.empty() ? "source_doc" : "source_" + SourceMatch.Field;
               BestHit.TermOrigin = BestHit.MatchedSource;
               BestHit.MatchedPath = "source_doc";
               BestHit.MatchedScore = EffectiveSourceScore;
               BestHit.MatchedSignal = EffectiveSourceScore;
               BestHit.Breakdown.TermScore = 0.0;
               BestHit.Breakdown.SourceDocScore = EffectiveSourceScore;
               BestHit.Breakdown.FinalScore = EffectiveSourceScore;

               if (IsSAM25DebugExplainEnabled())
               {
                    BestHit.Explain = SourceMatch.Explain;
               }
          }
          else if (BestScore > 0.0 && EffectiveSourceScore > 0.0)
          {
               const double MergeBonus = std::min(SourceDocMergeBonus, EffectiveSourceScore);
               BestHit.MatchedScore += MergeBonus;
               BestHit.Breakdown.SourceDocScore = EffectiveSourceScore;
               BestHit.Breakdown.SourceDocBonus += MergeBonus;
               BestHit.Breakdown.FinalScore = BestHit.MatchedScore;
               BestHit.MatchedPath = "hybrid";

               if (IsSAM25DebugExplainEnabled() && !BestHit.Explain.empty())
               {
                    std::ostringstream Stream;
                    Stream << BestHit.Explain
                           << " source_doc_bonus=" << MergeBonus;

                    if (!SourceMatch.Field.empty())
                    {
                         Stream << " source_doc_field=" << SourceMatch.Field;
                    }

                    if (!SourceMatch.Explain.empty())
                    {
                         Stream << " source_doc_explain={" << SourceMatch.Explain << "}";
                    }

                    BestHit.Explain = Stream.str();
               }
          }

          if (BestScore > 0.55 || EffectiveSourceScore > 0.0)
          {
               Candidates.push_back(std::move(BestHit));
          }
     }

     for (auto& Candidate : Candidates)
     {
          if (!Candidate.Collection.empty() && !Candidate.DocumentID.empty())
          {
               AccumulateSAMHit(AggregatedHits, Candidate);
          }
     }
}

/* Build the filesystem path for SAM's RocksDB side database. */

std::string ResolveSamDataDir()
{
     std::string SamDataDir = std::string(HLQUERY_SAM_DATA_DIR);
     const char* EnvSamDataDir = std::getenv("HLQUERY_SAM_DATA_DIR");

     if (EnvSamDataDir && *EnvSamDataDir)
     {
          SamDataDir = EnvSamDataDir;
     }

     try
     {
          if (Instance && Instance->Config && Instance->Config->IsValid())
          {
               const std::string& ConfiguredSamDataDir = Instance->Config->GetSamDataDirectory();

               if (!ConfiguredSamDataDir.empty())
               {
                    SamDataDir = ConfiguredSamDataDir;
               }
          }
     }
     catch (...)
     {
     }

     return SamDataDir;
}

/* Build the manifest key for one indexed SAM document. */

std::string BuildDocManifestKey(const std::string& Collection, const std::string& DocumentID)
{
     return "sam:doc:" + Collection + ":" + DocumentID;
}

std::string BuildSemanticProfileKey(const std::string& Term,
                                    const std::string& Collection,
                                    const std::string& DocumentID,
                                    const std::string& Kind)
{
     return "sam:semantic:" + NormalizeTerm(Term) + ":" + Collection + ":" + DocumentID + ":" + Kind;
}

std::string BuildSemanticProfilePrefix(const std::string& Term, const std::string& Collection)
{
     const std::string Normalized = NormalizeTerm(Term);
     return Collection.empty()
          ? "sam:semantic:" + Normalized + ":"
          : "sam:semantic:" + Normalized + ":" + Collection + ":";
}

/* Build the collection-level SAM profile key. */

std::string BuildCollectionProfileKey(const std::string& Collection)
{
     return "sam:profile:" + Collection;
}

std::string BuildSearchIdeaPrefix(const std::string& Collection)
{
     return "sam:idea:" + Collection + ":";
}

std::string BuildSearchIdeaKey(const std::string& Collection, const std::string& NormalizedQuery)
{
     return BuildSearchIdeaPrefix(Collection) + NormalizedQuery;
}

std::string BuildCollectionStateKey(const std::string& Collection)
{
     return "sam:state:" + Collection;
}

/* Read persisted collection rebuild/index state while the SAM DB lock is held. */

bool ReadCollectionStateLocked(rocksdb::DB* Database,
                               const std::string& Collection,
                               SAMCollectionState& State,
                               nlohmann::json* RootOut,
                               std::string* ErrorMessage)
{
     State = SAMCollectionState{};

     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection state lookup requires an open database and collection.";
          }

          return false;
     }

     std::string RawValue;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionStateKey(Collection), &RawValue);

     if (Status.IsNotFound())
     {
          if (RootOut)
          {
               *RootOut = nlohmann::json::object();
          }

          return true;
     }

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     try
     {
          nlohmann::json Root = nlohmann::json::parse(RawValue);

          if (Root.contains("indexed_mutation_version"))
          {
               if (Root["indexed_mutation_version"].is_number_unsigned())
               {
                    State.IndexedMutationVersion = Root["indexed_mutation_version"].get<uint64_t>();
                    State.HasIndexedMutationVersion = true;
               }
               else if (Root["indexed_mutation_version"].is_number_integer())
               {
                    const auto SignedVersion = Root["indexed_mutation_version"].get<long long>();
                    if (SignedVersion >= 0)
                    {
                         State.IndexedMutationVersion = static_cast<uint64_t>(SignedVersion);
                         State.HasIndexedMutationVersion = true;
                    }
               }
          }

          State.RebuildRequested = Root.value("rebuild_requested", false);

          if (Root.contains("rebuild_requested_mutation_version"))
          {
               if (Root["rebuild_requested_mutation_version"].is_number_unsigned())
               {
                    State.RequestedMutationVersion =
                         Root["rebuild_requested_mutation_version"].get<uint64_t>();
               }
               else if (Root["rebuild_requested_mutation_version"].is_number_integer())
               {
                    const auto SignedVersion =
                         Root["rebuild_requested_mutation_version"].get<long long>();
                    if (SignedVersion >= 0)
                    {
                         State.RequestedMutationVersion = static_cast<uint64_t>(SignedVersion);
                    }
               }
          }

          if (RootOut)
          {
               *RootOut = std::move(Root);
          }

          return true;
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }
     }

     return false;
}

/* Write collection rebuild/index state while the SAM DB lock is held. */

bool WriteCollectionStateLocked(rocksdb::DB* Database,
                                const std::string& Collection,
                                const SAMCollectionState& State,
                                std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection state write requires an open database and collection.";
          }

          return false;
     }

     nlohmann::json Root;
     Root["collection"] = Collection;
     if (State.HasIndexedMutationVersion)
     {
          Root["indexed_mutation_version"] = State.IndexedMutationVersion;
          Root["indexed_at_ms"] = Instance ? Instance->NowMs() : 0;
     }
     Root["rebuild_requested"] = State.RebuildRequested;
     if (State.RebuildRequested)
     {
          Root["rebuild_requested_mutation_version"] = State.RequestedMutationVersion;
          Root["rebuild_requested_at_ms"] = Instance ? Instance->NowMs() : 0;
     }

     const rocksdb::Status Status =
          Database->Put(rocksdb::WriteOptions(), BuildCollectionStateKey(Collection), Root.dump());

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     return true;
}

std::string BuildLexicalMirrorKey(const std::string& Kind, const std::string& Collection)
{
     return "sam:lexical:" + Kind + ":" + Collection;
}

size_t CountMirroredSynonymGroups(const nlohmann::json& Root)
{
     if (Root.is_object() && Root.contains("synonyms") && Root["synonyms"].is_array())
     {
          return Root["synonyms"].size();
     }

     if (Root.is_array())
     {
          return Root.size();
     }

     return 0;
}

size_t CountMirroredStopwords(const nlohmann::json& Root)
{
     if (Root.is_object() && Root.contains("stopwords") && Root["stopwords"].is_array())
     {
          return Root["stopwords"].size();
     }

     if (Root.is_array())
     {
          return Root.size();
     }

     return 0;
}

void LoadMirroredSynonymGraph(const nlohmann::json& Root,
                              std::unordered_map<std::string, std::vector<std::string>>& SynonymGraph)
{
     nlohmann::json Groups = nlohmann::json::array();

     if (Root.is_object() && Root.contains("synonyms") && Root["synonyms"].is_array())
     {
          Groups = Root["synonyms"];
     }
     else if (Root.is_array())
     {
          Groups = Root;
     }

     for (const auto& Group : Groups)
     {
          if (!Group.is_object())
          {
               continue;
          }

          std::vector<std::string> Terms;
          const std::string RootTerm = NormalizeTerm(Group.value("root", ""));

          if (!RootTerm.empty())
          {
               Terms.push_back(RootTerm);
          }

          if (Group.contains("synonyms") && Group["synonyms"].is_array())
          {
               for (const auto& Syn : Group["synonyms"])
               {
                    if (!Syn.is_string())
                    {
                         continue;
                    }

                    const std::string Normalized = NormalizeTerm(Syn.get<std::string>());

                    if (!Normalized.empty())
                    {
                         Terms.push_back(Normalized);
                    }
               }
          }

          std::sort(Terms.begin(), Terms.end());
          Terms.erase(std::unique(Terms.begin(), Terms.end()), Terms.end());

          for (const auto& Term : Terms)
          {
               auto& Targets = SynonymGraph[Term];

               for (const auto& Candidate : Terms)
               {
                    if (Candidate != Term)
                    {
                         Targets.push_back(Candidate);
                    }
               }

               std::sort(Targets.begin(), Targets.end());
               Targets.erase(std::unique(Targets.begin(), Targets.end()), Targets.end());
          }
     }
}

void LoadMirroredStopwords(const nlohmann::json& Root,
                           std::unordered_set<std::string>& Stopwords)
{
     nlohmann::json Values = nlohmann::json::array();

     if (Root.is_object() && Root.contains("stopwords") && Root["stopwords"].is_array())
     {
          Values = Root["stopwords"];
     }
     else if (Root.is_array())
     {
          Values = Root;
     }

     for (const auto& Entry : Values)
     {
          std::string Word;

          if (Entry.is_string())
          {
               Word = Entry.get<std::string>();
          }
          else if (Entry.is_object())
          {
               if (Entry.contains("word") && Entry["word"].is_string())
               {
                    Word = Entry["word"].get<std::string>();
               }
               else if (Entry.contains("text") && Entry["text"].is_string())
               {
                    Word = Entry["text"].get<std::string>();
               }
          }

          Word = NormalizeTerm(Word);

          if (!Word.empty())
          {
               Stopwords.insert(Word);
          }
     }
}

bool LoadMirroredLexicalJSON(rocksdb::DB* Database,
                             const std::string& Kind,
                             const std::string& Collection,
                             nlohmann::json& Root,
                             std::string* ErrorMessage = nullptr)
{
     Root = nlohmann::json::object();

     if (!Database || Collection.empty())
     {
          return false;
     }

     std::string RawValue;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildLexicalMirrorKey(Kind, Collection), &RawValue);

     if (Status.IsNotFound() || RawValue.empty())
     {
          return false;
     }

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     try
     {
          Root = nlohmann::json::parse(RawValue);
          return true;
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }
     }

     return false;
}

void LoadSAMSynonymGraphForCollection(rocksdb::DB* Database,
                                      const std::string& Collection,
                                      std::unordered_map<std::string, std::vector<std::string>>& SynonymGraph)
{
     if (!Database)
     {
          return;
     }

     auto LoadScope = [&](const std::string& Scope)
     {
          nlohmann::json Root;

          if (LoadMirroredLexicalJSON(Database, "synonyms", Scope, Root))
          {
               LoadMirroredSynonymGraph(Root, SynonymGraph);
          }
     };

     if (!Collection.empty())
     {
          LoadScope(Collection);
     }

     LoadScope(kSAMGlobalLexicalCollection);
}

void LoadSAMStopwordsForCollection(rocksdb::DB* Database,
                                   const std::string& Collection,
                                   std::unordered_set<std::string>& Stopwords)
{
     if (!Database)
     {
          return;
     }

     auto LoadScope = [&](const std::string& Scope)
     {
          nlohmann::json Root;

          if (LoadMirroredLexicalJSON(Database, "stopwords", Scope, Root))
          {
               LoadMirroredStopwords(Root, Stopwords);
          }
     };

     if (!Collection.empty())
     {
          LoadScope(Collection);
     }

     LoadScope(kSAMGlobalLexicalCollection);
}

constexpr size_t kSAMSearchIdeasMaxEntries = 100;
constexpr size_t kSAMSearchIdeaMaxDocs = 10;
constexpr uint64_t kSAMSearchIdeaRecentWindowMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kSAMSearchIdeaProfileMinUses = 2;
constexpr uint64_t kSAMSearchIdeaProfileSyncCooldownMs = 60ULL * 60ULL * 1000ULL;
constexpr size_t kSAMSearchIdeaProfileForceSyncMinDelta = 4;
uint64_t GetSAMCurrentTimeMS()
{
     return static_cast<uint64_t>(NowMs());
}

bool ShouldTrackSAMSearchIdeas(const std::string& Collection)
{
     if (Collection.empty())
     {
          return false;
     }

     return true;
}

double GetSAMIdeaFreshness(uint64_t LastSeenMS, uint64_t NowMS)
{
     if (LastSeenMS == 0 || NowMS <= LastSeenMS)
     {
          return 1.0;
     }

     const double AgeHours = static_cast<double>(NowMS - LastSeenMS) / (1000.0 * 60.0 * 60.0);
     return ClampSAMScore(1.0 / (1.0 + (AgeHours / 24.0)));
}

nlohmann::json SerializeSearchIdeaEntry(const SAM::SearchIdeaEntry& Entry)
{
     nlohmann::json Root;
     Root["collection"] = Entry.Collection;
     Root["query"] = Entry.Query;
     Root["normalized_query"] = Entry.NormalizedQuery;
     Root["first_seen_ms"] = Entry.FirstSeenMS;
     Root["last_seen_ms"] = Entry.LastSeenMS;
     Root["uses"] = Entry.Uses;
     Root["vector"] = nlohmann::json::array();
     Root["documents"] = nlohmann::json::array();
     Root["resolved_interpretation"] = Entry.ResolvedInterpretation;
     Root["resolved_conclusion"] = Entry.ResolvedConclusion;
     Root["resolved_at_ms"] = Entry.ResolvedAtMS;
     Root["resolved_uses"] = Entry.ResolvedUses;
     Root["interaction_uses"] = Entry.InteractionUses;
     Root["last_interaction_ms"] = Entry.LastInteractionMS;
     Root["resolved_candidates"] = nlohmann::json::array();
     Root["resolved_ranked_terms"] = nlohmann::json::array();

     for (float Value : Entry.Vector)
     {
          Root["vector"].push_back(Value);
     }

     for (const auto& Document : Entry.Documents)
     {
          Root["documents"].push_back({
               {"id", Document.DocumentID},
               {"title", Document.Title},
               {"score", Document.Score},
               {"interaction_uses", Document.InteractionUses},
               {"last_interaction_ms", Document.LastInteractionMS}
          });
     }

     for (const auto& Candidate : Entry.ResolvedCandidates)
     {
          Root["resolved_candidates"].push_back({
               {"text", Candidate.Text},
               {"weight", Candidate.Weight}
          });
     }

     for (const auto& RankedTerm : Entry.ResolvedRankedTerms)
     {
          Root["resolved_ranked_terms"].push_back({
               {"text", RankedTerm.Text},
               {"weight", RankedTerm.Weight}
          });
     }

     return Root;
}

bool ParseSearchIdeaEntry(const std::string& RawValue, SAM::SearchIdeaEntry& Entry)
{
     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);
          Entry = SAM::SearchIdeaEntry{};
          Entry.Collection = Root.value("collection", "");
          Entry.Query = TrimCopy(Root.value("query", ""));
          Entry.NormalizedQuery = NormalizeTerm(Root.value("normalized_query", Entry.Query));
          Entry.FirstSeenMS = Root.value("first_seen_ms", static_cast<uint64_t>(0));
          Entry.LastSeenMS = Root.value("last_seen_ms", static_cast<uint64_t>(0));
          Entry.Uses = Root.value("uses", static_cast<uint64_t>(0));
          Entry.ResolvedInterpretation = TrimCopy(Root.value("resolved_interpretation", ""));
          Entry.ResolvedConclusion = TrimCopy(Root.value("resolved_conclusion", ""));
          Entry.ResolvedAtMS = Root.value("resolved_at_ms", static_cast<uint64_t>(0));
          Entry.ResolvedUses = Root.value("resolved_uses", static_cast<uint64_t>(0));
          Entry.InteractionUses = Root.value("interaction_uses", static_cast<uint64_t>(0));
          Entry.LastInteractionMS = Root.value("last_interaction_ms", static_cast<uint64_t>(0));

          if (Root.contains("vector") && Root["vector"].is_array())
          {
               for (const auto& Value : Root["vector"])
               {
                    Entry.Vector.push_back(Value.get<float>());
               }
          }

          if (Root.contains("documents") && Root["documents"].is_array())
          {
               for (const auto& Item : Root["documents"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    SAM::SearchIdeaDocumentRef Document;
                    Document.DocumentID = Item.value("id", "");
                    Document.Title = Item.value("title", "");
                    Document.Score = Item.value("score", 0.0);
                    Document.InteractionUses = Item.value("interaction_uses", static_cast<uint64_t>(0));
                    Document.LastInteractionMS = Item.value("last_interaction_ms", static_cast<uint64_t>(0));

                    if (!Document.DocumentID.empty())
                    {
                         Entry.Documents.push_back(std::move(Document));
                    }
               }
          }

          if (Root.contains("resolved_candidates") && Root["resolved_candidates"].is_array())
          {
               for (const auto& Item : Root["resolved_candidates"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    llm::SearchIntentCandidate Candidate;
                    Candidate.Text = NormalizeTerm(Item.value("text", ""));
                    Candidate.Weight = ClampSAMScore(Item.value("weight", 0.0));

                    if (!Candidate.Text.empty())
                    {
                         Entry.ResolvedCandidates.push_back(std::move(Candidate));
                    }
               }
          }

          if (Root.contains("resolved_ranked_terms") && Root["resolved_ranked_terms"].is_array())
          {
               for (const auto& Item : Root["resolved_ranked_terms"])
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    llm::SearchIntentCandidate RankedTerm;
                    RankedTerm.Text = NormalizeTerm(Item.value("text", ""));
                    RankedTerm.Weight = ClampSAMScore(Item.value("weight", 0.0));

                    if (!RankedTerm.Text.empty())
                    {
                         Entry.ResolvedRankedTerms.push_back(std::move(RankedTerm));
                    }
               }
          }

          if (Entry.NormalizedQuery.empty())
          {
               Entry.NormalizedQuery = NormalizeTerm(Entry.Query);
          }

          if (Entry.Query.empty())
          {
               Entry.Query = Entry.NormalizedQuery;
          }

          if (Entry.Vector.empty() && !Entry.NormalizedQuery.empty())
          {
               Entry.Vector = BuildHashedSemanticVector({Entry.NormalizedQuery});
          }

          return !Entry.Collection.empty() && !Entry.NormalizedQuery.empty();
     }
     catch (...)
     {
          return false;
     }
}

std::string BuildStrongSearchIdeaPhrase(const std::string& Value, size_t MaxTokens)
{
     const std::vector<std::string> Tokens = NormalizeSAMTokens(Value, true);
     std::vector<std::string> StrongTokens;
     StrongTokens.reserve(Tokens.size());

     for (const auto& Token : Tokens)
     {
          const std::string Singular = SingularizeToken(Token);

          if (!IsStrongSAMVariantToken(Singular))
          {
               continue;
          }

          StrongTokens.push_back(Singular);

          if (StrongTokens.size() >= MaxTokens)
          {
               break;
          }
     }

     return JoinTokens(StrongTokens);
}

std::string BuildSearchIdeaDescriptorForSubject(const std::string& QueryPhrase,
                                                const std::string& Subject)
{
     const std::vector<std::string> QueryTokens = NormalizeSAMTokens(QueryPhrase, true);
     const std::vector<std::string> SubjectTokens = NormalizeSAMTokens(Subject, true);
     std::unordered_set<std::string> SubjectSet;

     for (const auto& Token : SubjectTokens)
     {
          SubjectSet.insert(SingularizeToken(Token));
     }

     std::vector<std::string> ExtraTokens;

     for (const auto& Token : QueryTokens)
     {
          const std::string Singular = SingularizeToken(Token);

          if (SubjectSet.find(Singular) != SubjectSet.end() || !IsStrongSAMVariantToken(Singular))
          {
               continue;
          }

          ExtraTokens.push_back(Singular);

          if (ExtraTokens.size() >= 3)
          {
               break;
          }
     }

     return JoinTokens(ExtraTokens);
}

double ComputeSearchIdeaProfilePromotionScore(const SAM::SearchIdeaEntry& Entry, uint64_t NowMS)
{
     const double Popularity = ClampSAMScore(std::log1p(static_cast<double>(Entry.Uses)) / std::log(12.0));
     const double Freshness = GetSAMIdeaFreshness(Entry.LastSeenMS, NowMS);
     const double DocConsensus = ClampSAMScore(static_cast<double>(std::min<size_t>(Entry.Documents.size(), 3)) / 3.0);
     return ClampSAMScore((Popularity * 0.46) + (Freshness * 0.34) + (DocConsensus * 0.20));
}

bool SearchIntentCandidateWeightGreater(const llm::SearchIntentCandidate& Left,
                                        const llm::SearchIntentCandidate& Right)
{
     if (Left.Weight != Right.Weight)
     {
          return Left.Weight > Right.Weight;
     }

     return Left.Text < Right.Text;
}

uint64_t GetLatestRecentSearchIdeaTimestampLocked(rocksdb::DB* Database,
                                                  const std::string& Collection,
                                                  size_t* RecentCount = nullptr)
{
     if (RecentCount)
     {
          *RecentCount = 0;
     }

     if (!Database || Collection.empty())
     {
          return 0;
     }

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     uint64_t LatestSeenMS = 0;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          if (Entry.LastSeenMS == 0 || NowMS <= Entry.LastSeenMS ||
              (NowMS - Entry.LastSeenMS) > kSAMSearchIdeaRecentWindowMs)
          {
               continue;
          }

          if (Entry.Uses < kSAMSearchIdeaProfileMinUses)
          {
               continue;
          }

          const std::string StrongPhrase = BuildStrongSearchIdeaPhrase(Entry.Query);

          if (StrongPhrase.empty())
          {
               continue;
          }

          LatestSeenMS = std::max(LatestSeenMS, Entry.LastSeenMS);

          if (RecentCount)
          {
               ++(*RecentCount);
          }
     }

     return LatestSeenMS;
}

std::vector<std::string> CollectSearchReinforcedDocumentIDsLocked(rocksdb::DB* Database,
                                                                  const std::string& Collection,
                                                                  uint64_t NowMS,
                                                                  size_t Limit)
{
     std::vector<std::string> DocumentIDs;

     if (!Database || Collection.empty() || NowMS == 0 || Limit == 0)
     {
          return DocumentIDs;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::unordered_map<std::string, double> RankedDocuments;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) ||
              Entry.LastSeenMS == 0 ||
              NowMS < Entry.LastSeenMS ||
              (NowMS - Entry.LastSeenMS) > kSAMSearchIdeaRecentWindowMs ||
              (Entry.Uses < kSAMSearchIdeaProfileMinUses && Entry.InteractionUses == 0) ||
              BuildStrongSearchIdeaPhrase(Entry.Query).empty())
          {
               continue;
          }

          for (const auto& Document : Entry.Documents)
          {
               if (Document.DocumentID.empty())
               {
                    continue;
               }

               const double Score = static_cast<double>(std::min<uint64_t>(Entry.Uses, 8)) +
                                    (static_cast<double>(std::min<uint64_t>(Entry.InteractionUses, 8)) * 2.0) +
                                    (static_cast<double>(std::min<uint64_t>(Document.InteractionUses, 8)) * 3.0) +
                                    ClampSAMScore(Document.Score);
               RankedDocuments[Document.DocumentID] =
                    std::max(RankedDocuments[Document.DocumentID], Score);
          }
     }

     std::vector<std::pair<std::string, double>> Ranked(RankedDocuments.begin(), RankedDocuments.end());
     std::sort(Ranked.begin(), Ranked.end(),
               [](const auto& Left, const auto& Right)
               {
                    if (Left.second != Right.second)
                    {
                         return Left.second > Right.second;
                    }

                    return Left.first < Right.first;
               });

     for (const auto& Entry : Ranked)
     {
          DocumentIDs.push_back(Entry.first);

          if (DocumentIDs.size() >= Limit)
          {
               break;
          }
     }

     if (DocumentIDs.size() < Limit)
     {
          const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
          std::unique_ptr<rocksdb::Iterator> ManifestIterator(Database->NewIterator(rocksdb::ReadOptions()));
          std::vector<std::string> ExplorationCandidates;

          for (ManifestIterator->Seek(ManifestPrefix);
               ManifestIterator->Valid() && ManifestIterator->key().starts_with(ManifestPrefix);
               ManifestIterator->Next())
          {
               SAM::DocumentEntry Entry;

               if (!ParseManifestValue(ManifestIterator->value().ToString(), Entry) ||
                   Entry.DocumentID.empty() ||
                   std::find(DocumentIDs.begin(), DocumentIDs.end(), Entry.DocumentID) != DocumentIDs.end())
               {
                    continue;
               }

               ExplorationCandidates.push_back(Entry.DocumentID);
          }

          if (!ExplorationCandidates.empty())
          {
               const size_t Index = static_cast<size_t>((NowMS / kSAMSearchIdeaProfileSyncCooldownMs) %
                                                        ExplorationCandidates.size());
               DocumentIDs.push_back(ExplorationCandidates[Index]);
          }
     }

     return DocumentIDs;
}

bool ReadCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                const std::string& Collection,
                                                uint64_t& Version,
                                                std::string* ErrorMessage = nullptr)
{
     Version = 0;
     SAMCollectionState State;

     if (!ReadCollectionStateLocked(Database, Collection, State, nullptr, ErrorMessage))
     {
          return false;
     }

     if (!State.HasIndexedMutationVersion)
     {
          return false;
     }

     Version = State.IndexedMutationVersion;
     return true;
}

bool WriteCollectionIndexedMutationVersionLocked(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 uint64_t Version,
                                                 std::string* ErrorMessage)
{
     SAMCollectionState State;
     std::string ReadError;

     if (!ReadCollectionStateLocked(Database, Collection, State, nullptr, &ReadError))
     {
          if (ErrorMessage)
          {
               *ErrorMessage = ReadError;
          }

          return false;
     }

     State.HasIndexedMutationVersion = true;
     State.IndexedMutationVersion = Version;
     State.RebuildRequested = false;
     State.RequestedMutationVersion = 0;
     return WriteCollectionStateLocked(Database, Collection, State, ErrorMessage);
}

/* Build a lookup term key for one collection/document pair. */

std::string BuildTermKey(const std::string& Term, const std::string& Collection, const std::string& DocumentID)
{
     return "sam:term:" + Term + ":" + Collection + ":" + DocumentID;
}

void MergeRecentSearchIdeasIntoCollectionProfileLocked(
     rocksdb::DB* Database,
     const std::string& Collection,
     std::unordered_map<std::string, SAMProfileEntry>& LearnedRankedTerms,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedRelatedCounts,
     std::unordered_map<std::string, SAMProfileFamily>& LearnedFamilies,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyAliasCounts,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyDescriptorCounts,
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& LearnedFamilyQueryCounts,
     uint64_t* LatestIdeaSeenMS = nullptr,
     size_t* LearnedIdeaCount = nullptr)
{
     if (LatestIdeaSeenMS)
     {
          *LatestIdeaSeenMS = 0;
     }

     if (LearnedIdeaCount)
     {
          *LearnedIdeaCount = 0;
     }

     if (!Database || Collection.empty())
     {
          return;
     }

     struct RankedIdea
     {
          SAM::SearchIdeaEntry Entry;
          std::string QueryPhrase;
          double Score = 0.0;
     };

     const uint64_t NowMS = GetSAMCurrentTimeMS();
     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::vector<RankedIdea> Ideas;

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix);
          Iterator->Next())
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) ||
              Entry.LastSeenMS == 0 ||
              NowMS <= Entry.LastSeenMS ||
              (NowMS - Entry.LastSeenMS) > kSAMSearchIdeaRecentWindowMs ||
              Entry.Uses < kSAMSearchIdeaProfileMinUses ||
              Entry.Documents.empty())
          {
               continue;
          }

          const std::string QueryPhrase = BuildStrongSearchIdeaPhrase(Entry.Query);

          if (QueryPhrase.empty() || !IsCollectionProfileCandidate(QueryPhrase))
          {
               continue;
          }

          const double Score = ComputeSearchIdeaProfilePromotionScore(Entry, NowMS);

          if (Score < 0.48)
          {
               continue;
          }

          Ideas.push_back({std::move(Entry), QueryPhrase, Score});
     }

     std::sort(Ideas.begin(), Ideas.end(),
               [](const RankedIdea& A, const RankedIdea& B)
               {
                    if (A.Score != B.Score)
                    {
                         return A.Score > B.Score;
                    }

                    if (A.Entry.Uses != B.Entry.Uses)
                    {
                         return A.Entry.Uses > B.Entry.Uses;
                    }

                    return A.Entry.LastSeenMS > B.Entry.LastSeenMS;
               });

     if (Ideas.size() > 24)
     {
          Ideas.resize(24);
     }

     for (const auto& Idea : Ideas)
     {
          if (LatestIdeaSeenMS)
          {
               *LatestIdeaSeenMS = std::max(*LatestIdeaSeenMS, Idea.Entry.LastSeenMS);
          }

          if (LearnedIdeaCount)
          {
               ++(*LearnedIdeaCount);
          }

          SAMProfileEntry& Ranked = LearnedRankedTerms[Idea.QueryPhrase];
          Ranked.Text = Idea.QueryPhrase;
          Ranked.Score += 0.22 + (Idea.Score * 0.34);
          Ranked.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));

          for (const auto& RankedTerm : Idea.Entry.ResolvedRankedTerms)
          {
               if (RankedTerm.Text.empty() || !IsCollectionProfileCandidate(RankedTerm.Text))
               {
                    continue;
               }

               SAMProfileEntry& ResolvedRanked = LearnedRankedTerms[RankedTerm.Text];
               ResolvedRanked.Text = RankedTerm.Text;
               ResolvedRanked.Score += 0.28 + (Idea.Score * 0.30) + (ClampSAMScore(RankedTerm.Weight) * 0.34);
               ResolvedRanked.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 5));
          }

          std::unordered_set<std::string> SeenSubjects;

          for (const auto& Candidate : Idea.Entry.ResolvedCandidates)
          {
               if (Candidate.Text.empty())
               {
                    continue;
               }

               SAMProfileFamily& CandidateFamily = LearnedFamilies[Candidate.Text];
               CandidateFamily.Subject = Candidate.Text;
               CandidateFamily.Score += 0.20 + (Idea.Score * 0.16) + (ClampSAMScore(Candidate.Weight) * 0.24);
               CandidateFamily.Support += static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));
               LearnedFamilyQueryCounts[Candidate.Text][Idea.QueryPhrase] +=
                    static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));
          }

          for (const auto& DocumentRef : Idea.Entry.Documents)
          {
               if (DocumentRef.DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status Status =
                    Database->Get(rocksdb::ReadOptions(),
                                  BuildDocManifestKey(Collection, DocumentRef.DocumentID),
                                  &ManifestValue);

               if (!Status.ok())
               {
                    continue;
               }

               SAM::DocumentEntry DocumentEntry;

               if (!ParseManifestValue(ManifestValue, DocumentEntry))
               {
                    continue;
               }

               const std::string AnchorSubject = SelectProfileAnchorSubject(DocumentEntry);

               if (AnchorSubject.empty())
               {
                    continue;
               }

               LearnedRelatedCounts[Idea.QueryPhrase][AnchorSubject] += 1;

               if (!SeenSubjects.insert(AnchorSubject).second)
               {
                    continue;
               }

               SAMProfileFamily& Family = LearnedFamilies[AnchorSubject];
               Family.Subject = AnchorSubject;
               Family.Score += 0.16 + (Idea.Score * 0.20);
               Family.Support += 1;
               LearnedFamilyQueryCounts[AnchorSubject][Idea.QueryPhrase] +=
                    static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 4));

               const std::string DescriptorCandidate =
                    BuildSearchIdeaDescriptorForSubject(Idea.QueryPhrase, AnchorSubject);

               if (!DescriptorCandidate.empty() && IsCollectionProfileCandidate(DescriptorCandidate))
               {
                    LearnedFamilyDescriptorCounts[AnchorSubject][DescriptorCandidate] +=
                         static_cast<size_t>(std::min<uint64_t>(Idea.Entry.Uses, 3));
                    LearnedRelatedCounts[Idea.QueryPhrase][DescriptorCandidate] += 1;
               }

               const std::string NormalizedTitle = NormalizeTerm(DocumentEntry.Title);

               if (!NormalizedTitle.empty() && NormalizedTitle != AnchorSubject)
               {
                    LearnedFamilyAliasCounts[AnchorSubject][NormalizedTitle] += 1;
               }
          }
     }
}

bool AdjustDocumentContextFeedbackLocked(rocksdb::DB* Database,
                                         const std::string& Collection,
                                         const std::string& DocumentID,
                                         const std::string& Query,
                                         bool Interaction,
                                         std::string* ErrorMessage)
{
     if (!Database || Collection.empty() || DocumentID.empty())
     {
          return false;
     }

     const std::string NormalizedQuery = NormalizeTerm(Query);

     if (NormalizedQuery.empty())
     {
          return true;
     }

     std::string RawValue;
     const std::string ContextKey = BuildDocumentContextKey(Collection, DocumentID);
     const rocksdb::Status ReadStatus =
          Database->Get(rocksdb::ReadOptions(), ContextKey, &RawValue);

     if (ReadStatus.IsNotFound())
     {
          return true;
     }

     if (!ReadStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = ReadStatus.ToString();
          }

          return false;
     }

     try
     {
          nlohmann::json Root = nlohmann::json::parse(RawValue);

          if (!Root.contains("suggestions") || !Root["suggestions"].is_array())
          {
               return true;
          }

          const std::vector<std::string> QueryTokens = TokenizeNormalized(NormalizedQuery);
          bool Changed = false;
          bool AnyMatched = false;

          for (auto& Suggestion : Root["suggestions"])
          {
               if (!Suggestion.is_object() || Suggestion.value("kind", "") != "llm")
               {
                    continue;
               }

               const std::string Term = NormalizeTerm(Suggestion.value("term", ""));
               const std::vector<std::string> TermTokens = TokenizeNormalized(Term);
               size_t Overlap = 0;

               for (const auto& Token : QueryTokens)
               {
                    if (std::find(TermTokens.begin(), TermTokens.end(), Token) != TermTokens.end())
                    {
                         ++Overlap;
                    }
               }

               const bool Matched =
                    !Term.empty() &&
                    (Term.find(NormalizedQuery) != std::string::npos ||
                     NormalizedQuery.find(Term) != std::string::npos ||
                     (Overlap > 0 && Overlap * 2 >= std::min(QueryTokens.size(), TermTokens.size())));
               double Confidence =
                    std::clamp(Suggestion.value("confidence", 0.70), 0.0, 1.0);

               if (Matched)
               {
                    AnyMatched = true;
                    Suggestion["feedback_uses"] = Suggestion.value("feedback_uses", 0U) + 1;

                    if (Interaction)
                    {
                         Suggestion["interaction_uses"] = Suggestion.value("interaction_uses", 0U) + 1;
                    }

                    Suggestion["last_used_ms"] = GetSAMCurrentTimeMS();
                    Suggestion["misses"] = 0;
                    Confidence = std::min(1.0, Confidence + (Interaction ? 0.08 : 0.02));
               }
               else
               {
                    const size_t Misses = Suggestion.value("misses", 0U) + 1;
                    Suggestion["misses"] = Misses;

                    if (Misses % 8 == 0)
                    {
                         Confidence = std::max(0.35, Confidence - 0.03);
                    }
               }

               Suggestion["confidence"] = Confidence;
               Suggestion["provisional"] = Confidence < 0.78;
               Changed = true;
          }

          if (!Changed)
          {
               return true;
          }

          Root["feedback_updated_at_ms"] = GetSAMCurrentTimeMS();
          Root["feedback_matches"] = Root.value("feedback_matches", 0U) + (AnyMatched ? 1 : 0);
          Root["feedback_misses"] = Root.value("feedback_misses", 0U) + (AnyMatched ? 0 : 1);

          if (!AnyMatched)
          {
               const uint64_t WeakRevisitMS =
                    GetSAMCurrentTimeMS() + (7ULL * 24ULL * 60ULL * 60ULL * 1000ULL);
               const uint64_t ExistingRevisitMS =
                    Root.value("revisit_after_ms", static_cast<uint64_t>(0));
               Root["revisit_after_ms"] =
                    ExistingRevisitMS == 0 ? WeakRevisitMS : std::min(ExistingRevisitMS, WeakRevisitMS);
          }

          const rocksdb::Status WriteStatus =
               Database->Put(rocksdb::WriteOptions(), ContextKey, Root.dump());

          if (!WriteStatus.ok() && ErrorMessage)
          {
               *ErrorMessage = WriteStatus.ToString();
          }

          return WriteStatus.ok();
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage)
          {
               *ErrorMessage = E.what();
          }

          return false;
     }
}

/* Rebuild a collection profile from the current indexed SAM manifests and search ideas. */

bool RebuildCollectionProfileLocked(rocksdb::DB* Database,
                                    const std::string& Collection,
                                    std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection profile rebuild requires an open database and collection.";
          }

          return false;
     }

     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::unordered_map<std::string, SAMProfileEntry> RankedTerms;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> RelatedCounts;
     std::unordered_map<std::string, SAMProfileFamily> Families;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyAliasCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyDescriptorCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> FamilyQueryCounts;
     std::unordered_map<std::string, SAMProfileEntry> LearnedRankedTerms;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedRelatedCounts;
     std::unordered_map<std::string, SAMProfileFamily> LearnedFamilies;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyAliasCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyDescriptorCounts;
     std::unordered_map<std::string, std::unordered_map<std::string, size_t>> LearnedFamilyQueryCounts;
     size_t DocumentCount = 0;
     std::string ExistingLLMSummary;
     uint64_t ExistingLLMSummaryUpdatedAtMS = 0;
     std::string ExistingRawProfile;

     if (Database->Get(rocksdb::ReadOptions(),
                       BuildCollectionProfileKey(Collection),
                       &ExistingRawProfile).ok() &&
         !ExistingRawProfile.empty())
     {
          try
          {
               const nlohmann::json ExistingProfile = nlohmann::json::parse(ExistingRawProfile);
               ExistingLLMSummary = ExistingProfile.value("llm_summary", "");
               ExistingLLMSummaryUpdatedAtMS =
                    ExistingProfile.value("llm_summary_updated_at_ms", static_cast<uint64_t>(0));
          }
          catch (...)
          {
          }
     }

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix);
          Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry) || Entry.Terms.empty())
          {
               continue;
          }

          const std::string AnchorSubject = SelectProfileAnchorSubject(Entry);
          std::unordered_set<std::string> AliasTerms;
          std::unordered_set<std::string> DescriptorTerms;
          std::vector<std::pair<std::string, double>> RankedDescriptors;

          if (!AnchorSubject.empty())
          {
               AliasTerms.insert(AnchorSubject);
          }

          const std::string NormalizedTitle = NormalizeTerm(Entry.Title);

          if (!NormalizedTitle.empty())
          {
               AliasTerms.insert(NormalizedTitle);
          }

          ++DocumentCount;
          std::vector<std::string> SeedTerms;
          std::unordered_set<std::string> SeenInDoc;
          size_t AddedSeeds = 0;

          for (const auto& Term : Entry.Terms)
          {
               const std::string Candidate = NormalizeTerm(Term.Text);

               if (Candidate.empty())
               {
                    continue;
               }

               if (IsSubjectLikeTermKind(Term.Kind) && (Candidate == AnchorSubject || IsCollectionProfileCandidate(Candidate)))
               {
                    AliasTerms.insert(Candidate);
               }
               else if (IsDescriptorLikeTermKind(Term.Kind) && IsCollectionProfileCandidate(Candidate))
               {
                    DescriptorTerms.insert(Candidate);
                    RankedDescriptors.emplace_back(
                         Candidate,
                         (ClampSAMScore(Term.Score) * 0.65) +
                         (ClampSAMScore(Term.Signal) * 0.25) +
                         (GetCollectionProfileKindWeight(Term.Kind) * 0.10));
               }

               if (!IsCollectionProfileCandidate(Candidate) || !SeenInDoc.insert(Candidate).second)
               {
                    continue;
               }

               SAMProfileEntry& Ranked = RankedTerms[Candidate];
               Ranked.Text = Candidate;
               Ranked.Score += (ClampSAMScore(Term.Score) * 0.70) + (ClampSAMScore(Term.Signal) * 0.30);
               ++Ranked.Support;
               SeedTerms.push_back(Candidate);
               ++AddedSeeds;

               if (AddedSeeds >= 8)
               {
                    break;
               }
          }

          if (!AnchorSubject.empty())
          {
               SAMProfileFamily& Family = Families[AnchorSubject];
               Family.Subject = AnchorSubject;
               Family.Score += 0.78;
               ++Family.Support;

               for (const auto& Alias : AliasTerms)
               {
                    if (!Alias.empty() && Alias != AnchorSubject)
                    {
                         ++FamilyAliasCounts[AnchorSubject][Alias];
                    }
               }

               std::sort(RankedDescriptors.begin(), RankedDescriptors.end(),
                         [](const auto& A, const auto& B)
                         {
                              if (A.second != B.second)
                              {
                                   return A.second > B.second;
                              }

                              return A.first < B.first;
                         });

               size_t DescriptorLimit = 0;

               for (const auto& RankedDescriptor : RankedDescriptors)
               {
                    if (!DescriptorTerms.contains(RankedDescriptor.first))
                    {
                         continue;
                    }

                    ++FamilyDescriptorCounts[AnchorSubject][RankedDescriptor.first];
                    ++DescriptorLimit;

                    if (DescriptorLimit >= 8)
                    {
                         break;
                    }
               }

               DescriptorLimit = 0;

               for (const auto& RankedDescriptor : RankedDescriptors)
               {
                    if (!DescriptorTerms.contains(RankedDescriptor.first))
                    {
                         continue;
                    }

                    const std::string QueryCandidate = AnchorSubject + " " + RankedDescriptor.first;

                    if (IsCollectionProfileCandidate(QueryCandidate))
                    {
                         ++FamilyQueryCounts[AnchorSubject][QueryCandidate];
                    }

                    ++DescriptorLimit;

                    if (DescriptorLimit >= 5)
                    {
                         break;
                    }
               }
          }

          for (size_t I = 0; I < SeedTerms.size(); ++I)
          {
               for (size_t J = 0; J < SeedTerms.size(); ++J)
               {
                    if (I == J)
                    {
                         continue;
                    }

                    ++RelatedCounts[SeedTerms[I]][SeedTerms[J]];
               }
          }
     }

     nlohmann::json Profile;
     Profile["collection"] = Collection;
     Profile["documents"] = DocumentCount;
     Profile["terms"] = nlohmann::json::array();
     Profile["families"] = nlohmann::json::array();
     Profile["learned_terms"] = nlohmann::json::array();
     Profile["learned_families"] = nlohmann::json::array();
     uint64_t LatestIdeaSeenMS = 0;
     size_t LearnedIdeaCount = 0;
     const uint64_t ProfileSyncedAtMS = GetSAMCurrentTimeMS();
     Profile["profile_synced_at_ms"] = ProfileSyncedAtMS;

     if (!ExistingLLMSummary.empty())
     {
          Profile["llm_summary"] = ExistingLLMSummary;
          Profile["llm_summary_updated_at_ms"] = ExistingLLMSummaryUpdatedAtMS;
     }
     size_t RecentIdeaCount = 0;
     (void)GetLatestRecentSearchIdeaTimestampLocked(Database, Collection, &RecentIdeaCount);
     MergeRecentSearchIdeasIntoCollectionProfileLocked(Database,
                                                       Collection,
                                                       LearnedRankedTerms,
                                                       LearnedRelatedCounts,
                                                       LearnedFamilies,
                                                       LearnedFamilyAliasCounts,
                                                       LearnedFamilyDescriptorCounts,
                                                       LearnedFamilyQueryCounts,
                                                       &LatestIdeaSeenMS,
                                                       &LearnedIdeaCount);
     Profile["idea_sync_marker_ms"] = LatestIdeaSeenMS;
     Profile["idea_terms_merged"] = LearnedIdeaCount;
     Profile["idea_profile_synced_at_ms"] = ProfileSyncedAtMS;
     Profile["idea_recent_count"] = RecentIdeaCount;
     const size_t MinTermSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinRelatedSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilySupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyDescriptorSupport = DocumentCount <= 8 ? 1 : 2;
     const size_t MinFamilyAliasSupport = 1;
     const size_t MinFamilyQuerySupport = 1;

     auto BuildSortedTerms = [&](std::unordered_map<std::string, SAMProfileEntry>& SourceTerms,
                                 std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceRelated)
     {
          std::vector<SAMProfileEntry> SortedTerms;
          SortedTerms.reserve(SourceTerms.size());

          for (auto& Pair : SourceTerms)
          {
               SAMProfileEntry Entry = Pair.second;

               if (Entry.Support < MinTermSupport)
               {
                    continue;
               }

               auto RelatedIt = SourceRelated.find(Entry.Text);

               if (RelatedIt != SourceRelated.end())
               {
                    std::vector<std::pair<std::string, size_t>> Related(RelatedIt->second.begin(), RelatedIt->second.end());
                    std::sort(Related.begin(), Related.end(),
                              [](const auto& A, const auto& B)
                              {
                                   if (A.second != B.second)
                                   {
                                        return A.second > B.second;
                                   }

                                   return A.first < B.first;
                              });

                    for (const auto& RelatedPair : Related)
                    {
                         if (RelatedPair.second < MinRelatedSupport)
                         {
                              continue;
                         }

                         Entry.Related.push_back(RelatedPair.first);

                         if (Entry.Related.size() >= 6)
                         {
                              break;
                         }
                    }
               }

               Entry.Score = ClampSAMScore(Entry.Score / static_cast<double>(std::max<size_t>(1, Entry.Support)));
               SortedTerms.push_back(std::move(Entry));
          }

          std::sort(SortedTerms.begin(), SortedTerms.end(),
                    [](const SAMProfileEntry& A, const SAMProfileEntry& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         if (A.Text.size() != B.Text.size())
                         {
                              return A.Text.size() < B.Text.size();
                         }

                         return A.Text < B.Text;
                    });

          if (SortedTerms.size() > 48)
          {
               SortedTerms.resize(48);
          }

          return SortedTerms;
     };

     auto AppendRankedValues = [](const auto& Source, std::vector<std::string>& Output, size_t MinSupport, size_t MaxItems)
     {
          std::vector<std::pair<std::string, size_t>> Ranked(Source.begin(), Source.end());
          std::sort(Ranked.begin(), Ranked.end(),
                    [](const auto& A, const auto& B)
                    {
                         if (A.second != B.second)
                         {
                              return A.second > B.second;
                         }

                         return A.first < B.first;
                    });

          for (const auto& Item : Ranked)
          {
               if (Item.second < MinSupport)
               {
                    continue;
               }

               Output.push_back(Item.first);

               if (Output.size() >= MaxItems)
               {
                    break;
               }
          }
     };

     auto BuildSortedFamilies = [&](std::unordered_map<std::string, SAMProfileFamily>& SourceFamilies,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyAliasCounts,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyDescriptorCounts,
                                    std::unordered_map<std::string, std::unordered_map<std::string, size_t>>& SourceFamilyQueryCounts)
     {
          std::vector<SAMProfileFamily> SortedFamilies;
          SortedFamilies.reserve(SourceFamilies.size());

          for (auto& Pair : SourceFamilies)
          {
               SAMProfileFamily Family = Pair.second;

               if (Family.Support < MinFamilySupport)
               {
                    continue;
               }

               if (const auto AliasIt = SourceFamilyAliasCounts.find(Family.Subject); AliasIt != SourceFamilyAliasCounts.end())
               {
                    AppendRankedValues(AliasIt->second, Family.Aliases, MinFamilyAliasSupport, 6);
               }

               if (const auto DescriptorIt = SourceFamilyDescriptorCounts.find(Family.Subject);
                   DescriptorIt != SourceFamilyDescriptorCounts.end())
               {
                    AppendRankedValues(DescriptorIt->second, Family.Descriptors, MinFamilyDescriptorSupport, 8);
               }

               if (const auto QueryIt = SourceFamilyQueryCounts.find(Family.Subject); QueryIt != SourceFamilyQueryCounts.end())
               {
                    AppendRankedValues(QueryIt->second, Family.Queries, MinFamilyQuerySupport, 8);
               }

               Family.Score = ClampSAMScore(Family.Score / static_cast<double>(std::max<size_t>(1, Family.Support)));
               SortedFamilies.push_back(std::move(Family));
          }

          std::sort(SortedFamilies.begin(), SortedFamilies.end(),
                    [](const SAMProfileFamily& A, const SAMProfileFamily& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         return A.Subject < B.Subject;
                    });

          if (SortedFamilies.size() > 24)
          {
               SortedFamilies.resize(24);
          }

          return SortedFamilies;
     };

     auto AppendTermsToProfile = [&](const std::vector<SAMProfileEntry>& SortedTerms, const char* Key)
     {
          for (const auto& Entry : SortedTerms)
          {
               Profile[Key].push_back({
                    {"text", Entry.Text},
                    {"score", Entry.Score},
                    {"support", Entry.Support},
                    {"related", Entry.Related}
               });
          }
     };

     auto AppendFamiliesToProfile = [&](const std::vector<SAMProfileFamily>& SortedFamilies, const char* Key)
     {
          for (const auto& Family : SortedFamilies)
          {
               Profile[Key].push_back({
                    {"subject", Family.Subject},
                    {"score", Family.Score},
                    {"support", Family.Support},
                    {"aliases", Family.Aliases},
                    {"descriptors", Family.Descriptors},
                    {"queries", Family.Queries}
               });
          }
     };

     // Basic collection stats to allow other components to gate behavior until
     // a real profile has been built (e.g. suppressing synthetic term spam).
     Profile["document_count"] = static_cast<int64_t>(DocumentCount);

     const std::vector<SAMProfileEntry> SortedTerms = BuildSortedTerms(RankedTerms, RelatedCounts);
     const std::vector<SAMProfileFamily> SortedFamilies =
          BuildSortedFamilies(Families,
                              FamilyAliasCounts,
                              FamilyDescriptorCounts,
                              FamilyQueryCounts);
     const std::vector<SAMProfileEntry> SortedLearnedTerms =
          BuildSortedTerms(LearnedRankedTerms, LearnedRelatedCounts);
     const std::vector<SAMProfileFamily> SortedLearnedFamilies =
          BuildSortedFamilies(LearnedFamilies,
                              LearnedFamilyAliasCounts,
                              LearnedFamilyDescriptorCounts,
                              LearnedFamilyQueryCounts);

     AppendTermsToProfile(SortedTerms, "terms");
     AppendFamiliesToProfile(SortedFamilies, "families");
     AppendTermsToProfile(SortedLearnedTerms, "learned_terms");
     AppendFamiliesToProfile(SortedLearnedFamilies, "learned_families");

     Profile["summary_terms"] = nlohmann::json::array();
     Profile["negative_terms"] = nlohmann::json::array();
     std::vector<std::string> SummaryTerms;
     std::unordered_set<std::string> SeenSummaryTerms;

     auto AppendSummaryTerm = [&](const std::string& Value)
     {
          const std::string Normalized = NormalizeTerm(Value);

          if (Normalized.empty() || !SeenSummaryTerms.insert(Normalized).second)
          {
               return;
          }

          SummaryTerms.push_back(Normalized);
     };

     for (const auto& Family : SortedFamilies)
     {
          AppendSummaryTerm(Family.Subject);

          if (SummaryTerms.size() >= 4)
          {
               break;
          }
     }

     for (const auto& Entry : SortedTerms)
     {
          if (DocumentCount > 0 &&
              Entry.Support * 100 >= DocumentCount * 65)
          {
               Profile["negative_terms"].push_back(Entry.Text);
               continue;
          }

          AppendSummaryTerm(Entry.Text);

          if (SummaryTerms.size() >= 8)
          {
               break;
          }
     }

     for (const auto& Entry : SortedLearnedTerms)
     {
          AppendSummaryTerm(Entry.Text);

          if (SummaryTerms.size() >= 8)
          {
               break;
          }
     }

     for (const auto& Term : SummaryTerms)
     {
          Profile["summary_terms"].push_back(Term);
     }

     std::ostringstream Summary;
     Summary << "Collection '" << Collection << "' contains " << DocumentCount << " indexed document(s)";

     if (!SummaryTerms.empty())
     {
          Summary << " centered on ";

          for (size_t Index = 0; Index < SummaryTerms.size(); ++Index)
          {
               if (Index > 0)
               {
                    Summary << ", ";
               }

               Summary << SummaryTerms[Index];
          }
     }

     Profile["summary"] = Summary.str();

     const rocksdb::Status Status =
          Database->Put(rocksdb::WriteOptions(), BuildCollectionProfileKey(Collection), Profile.dump());

     if (!Status.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = Status.ToString();
          }

          return false;
     }

     if (!RebuildIntentGraphLocked(Database, Collection, ErrorMessage))
     {
          return false;
     }

     return true;
}

std::vector<std::string> BuildPersistedCollectionProfileVariants(rocksdb::DB* Database,
                                                                 const std::string& Collection,
                                                                 const SAMQueryTokenViews& QueryViews,
                                                                 const std::unordered_set<std::string>* StrongTokens = nullptr,
                                                                 size_t MaxVariants = 12)
{
     std::vector<std::string> Variants;

     if (!Database || Collection.empty() || QueryViews.CoreTokens.empty() || MaxVariants == 0 ||
         IsSingleTokenSAMIntent(QueryViews))
     {
          return Variants;
     }

     std::string RawProfile;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (!Status.ok() || RawProfile.empty())
     {
          return Variants;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawProfile);

          const bool HasTerms = Root.contains("terms") && Root["terms"].is_array();
          const bool HasLearnedTerms = Root.contains("learned_terms") && Root["learned_terms"].is_array();
          const bool HasFamilies = Root.contains("families") && Root["families"].is_array();
          const bool HasLearnedFamilies = Root.contains("learned_families") && Root["learned_families"].is_array();

          if (!HasTerms && !HasLearnedTerms && !HasFamilies && !HasLearnedFamilies)
          {
               return Variants;
          }

          std::unordered_set<std::string> QueryTokens(QueryViews.CoreTokens.begin(), QueryViews.CoreTokens.end());
          std::unordered_map<std::string, SAMLearnedVariant> Ranked;

          auto ProcessTermArray = [&](const nlohmann::json& TermsArray, double LayerWeight)
          {
               for (const auto& Item : TermsArray)
               {
                    if (!Item.is_object())
                    {
                         continue;
                    }

                    const std::string Text = NormalizeTerm(Item.value("text", ""));

                    if (Text.empty())
                    {
                         continue;
                    }

                    const double EntryScore = ClampSAMScore(Item.value("score", 0.0) * LayerWeight);
                    const size_t Support = static_cast<size_t>(std::max<int64_t>(0, Item.value("support", 0)));
                    const double MatchStrength =
                         IsUsefulLearnedVariant(Text, QueryTokens, StrongTokens) ? 0.0 :
                         std::max(ComputeManifestSeedStrength(
                                       QueryViews,
                                       SAM::TermEntry{Text, "collection_profile", "profile", EntryScore, EntryScore}),
                                  0.0);

                    if (MatchStrength < (LayerWeight < 1.0 ? 0.62 : 0.58))
                    {
                         continue;
                    }

                    if (Item.contains("related") && Item["related"].is_array())
                    {
                         for (const auto& RelatedItem : Item["related"])
                         {
                              if (!RelatedItem.is_string())
                              {
                                   continue;
                              }

                              const std::string Candidate = NormalizeTerm(RelatedItem.get<std::string>());

                              if (!IsUsefulLearnedVariant(Candidate, QueryTokens, StrongTokens))
                              {
                                   continue;
                              }

                              SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                              RankedEntry.Text = Candidate;
                              RankedEntry.Score += (MatchStrength * (0.60 * LayerWeight)) + (EntryScore * 0.25) +
                                                   ClampSAMScore(static_cast<double>(Support) / 8.0) * 0.15;
                              ++RankedEntry.Support;
                         }
                    }
               }
          };

          auto ProcessFamilyArray = [&](const nlohmann::json& FamiliesArray, double LayerWeight)
          {
               for (const auto& FamilyItem : FamiliesArray)
               {
                    if (!FamilyItem.is_object())
                    {
                         continue;
                    }

                    const std::string Subject = NormalizeTerm(FamilyItem.value("subject", ""));

                    if (Subject.empty())
                    {
                         continue;
                    }

                    const double FamilyScore = ClampSAMScore(FamilyItem.value("score", 0.0) * LayerWeight);
                    const size_t FamilySupport =
                         static_cast<size_t>(std::max<int64_t>(0, FamilyItem.value("support", 0)));

                    std::vector<std::string> MatchTerms;
                    MatchTerms.push_back(Subject);

                    if (FamilyItem.contains("aliases") && FamilyItem["aliases"].is_array())
                    {
                         for (const auto& AliasItem : FamilyItem["aliases"])
                         {
                              if (AliasItem.is_string())
                              {
                                   MatchTerms.push_back(NormalizeTerm(AliasItem.get<std::string>()));
                              }
                         }
                    }

                    if (FamilyItem.contains("descriptors") && FamilyItem["descriptors"].is_array())
                    {
                         for (const auto& DescriptorItem : FamilyItem["descriptors"])
                         {
                              if (DescriptorItem.is_string())
                              {
                                   MatchTerms.push_back(NormalizeTerm(DescriptorItem.get<std::string>()));
                              }
                         }
                    }

                    double FamilyMatch = 0.0;

                    for (const auto& MatchTerm : MatchTerms)
                    {
                         if (MatchTerm.empty())
                         {
                              continue;
                         }

                         FamilyMatch = std::max(
                              FamilyMatch,
                              std::max(ComputeManifestSeedStrength(
                                            QueryViews,
                                            SAM::TermEntry{MatchTerm, "collection_profile", "profile", FamilyScore, FamilyScore}),
                                       0.0));
                    }

                    if (FamilyMatch < (LayerWeight < 1.0 ? 0.60 : 0.56))
                    {
                         continue;
                    }

                    auto AccumulateVariant = [&](const std::string& CandidateText, double Weight)
                    {
                         const std::string Candidate = NormalizeTerm(CandidateText);

                         if (!IsUsefulLearnedVariant(Candidate, QueryTokens, StrongTokens))
                         {
                              return;
                         }

                         SAMLearnedVariant& RankedEntry = Ranked[Candidate];
                         RankedEntry.Text = Candidate;
                         RankedEntry.Score += (FamilyMatch * (Weight * LayerWeight)) +
                                              (FamilyScore * 0.22) +
                                              ClampSAMScore(static_cast<double>(FamilySupport) / 8.0) * 0.14;
                         ++RankedEntry.Support;
                    };

                    AccumulateVariant(Subject, 0.46);

                    if (FamilyItem.contains("aliases") && FamilyItem["aliases"].is_array())
                    {
                         for (const auto& AliasItem : FamilyItem["aliases"])
                         {
                              if (AliasItem.is_string())
                              {
                                   AccumulateVariant(AliasItem.get<std::string>(), 0.44);
                              }
                         }
                    }

                    if (FamilyItem.contains("descriptors") && FamilyItem["descriptors"].is_array())
                    {
                         for (const auto& DescriptorItem : FamilyItem["descriptors"])
                         {
                              if (DescriptorItem.is_string())
                              {
                                   AccumulateVariant(DescriptorItem.get<std::string>(), 0.52);
                              }
                         }
                    }

                    if (FamilyItem.contains("queries") && FamilyItem["queries"].is_array())
                    {
                         for (const auto& QueryItem : FamilyItem["queries"])
                         {
                              if (QueryItem.is_string())
                              {
                                   AccumulateVariant(QueryItem.get<std::string>(), 0.60);
                              }
                         }
                    }
               }
          };

          if (HasTerms)
          {
               ProcessTermArray(Root["terms"], 1.0);
          }

          if (HasLearnedTerms)
          {
               ProcessTermArray(Root["learned_terms"], 0.78);
          }

          if (HasFamilies)
          {
               ProcessFamilyArray(Root["families"], 1.0);
          }

          if (HasLearnedFamilies)
          {
               ProcessFamilyArray(Root["learned_families"], 0.80);
          }

          std::vector<SAMLearnedVariant> Sorted;
          Sorted.reserve(Ranked.size());

          for (const auto& Pair : Ranked)
          {
               if (Pair.second.Score >= 0.62)
               {
                    Sorted.push_back(Pair.second);
               }
          }

          std::sort(Sorted.begin(), Sorted.end(),
                    [](const SAMLearnedVariant& A, const SAMLearnedVariant& B)
                    {
                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         return A.Text < B.Text;
                    });

          for (const auto& Entry : Sorted)
          {
               Variants.push_back(Entry.Text);

               if (Variants.size() >= MaxVariants)
               {
                    break;
               }
          }
     }
     catch (...)
     {
     }

     return Variants;
}

/* Parse a persisted SAM manifest into a document entry. */

bool ParseManifestValue(const std::string& RawValue, SAM::DocumentEntry& Entry)
{
     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawValue);
          SAMSemanticProfile SemanticProfile;
          Entry.Collection = Root.value("collection", "");
          Entry.DocumentID = Root.value("id", "");
          Entry.Title = Root.value("title", "");
          Entry.SourceTimestamp = Root.value("source_timestamp", 0ULL);
          Entry.SourceFingerprint = Root.value("source_fingerprint", "");
          Entry.Lang = Root.value("lang", "und");
          Entry.Label = Root.value("label", "article");
          Entry.Format = Root.value("format", "text");
          Entry.Subject.clear();
          Entry.Summary.clear();
          Entry.Aliases.clear();
          Entry.Descriptors.clear();
          Entry.Queries.clear();
          Entry.Terms.clear();

          if (Root.contains("terms") && Root["terms"].is_array())
          {
               for (const auto& Term : Root["terms"])
               {
                    if (Term.is_string())
                    {
                         SAM::TermEntry Item;
                         Item.Text = Term.get<std::string>();
                         Item.Kind = "legacy";
                         Item.Score = 0.5;
                         Entry.Terms.push_back(std::move(Item));
                    }
                    else if (Term.is_object())
                    {
                         SAM::TermEntry Item;
                         Item.Text = Term.value("text", "");
                         Item.Kind = Term.value("kind", "");
                         Item.Source = Term.value("source", "");
                         Item.Score = Term.value("score", 0.0);
                         Item.Signal = Term.value("signal", 0.0);

                         if (!Item.Text.empty())
                         {
                              Entry.Terms.push_back(std::move(Item));
                         }
                    }
               }
          }

          std::sort(Entry.Terms.begin(), Entry.Terms.end(),
                    [](const SAM::TermEntry& A, const SAM::TermEntry& B)
                    {
                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         if (A.Signal != B.Signal)
                         {
                              return A.Signal > B.Signal;
                         }

                         return A.Text < B.Text;
                    });

          if (ParseSemanticProfileJSON(Root, SemanticProfile))
          {
               Entry.Subject = SemanticProfile.Subject;
               Entry.Summary = SemanticProfile.Summary;
               Entry.Aliases = std::move(SemanticProfile.Aliases);
               Entry.Descriptors = std::move(SemanticProfile.Descriptors);
               Entry.Queries = std::move(SemanticProfile.Queries);
          }

          return !Entry.Collection.empty() && !Entry.DocumentID.empty();
     }
     catch (...)
     {
          return false;
     }
}

#endif /* HLQUERY_SAM_SPLIT_INCLUDE */
