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

#include <iostream>
#include <string>
#include <vector>

#include "app.h"

void HLQueryCLI::ShowExamples()
{
     const size_t box_width = 68;
     const std::string &program_name = ProgramName;

     auto PrintBoxLine = [&](const std::string &content)
     {
          std::string line = content;

          if (line.size() < box_width)
          {
               line += std::string(box_width - line.size(), ' ');
          }
          else if (line.size() > box_width)
          {
               line = line.substr(0, box_width);
          }

          std::cout << "║" << line << "║\n";
     };

     std::cout << "\n";
     std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
     PrintBoxLine("");
     PrintBoxLine("  HLQUERY CLI - USAGE EXAMPLES");
     PrintBoxLine("");
     std::cout << "╠════════════════════════════════════════════════════════════════════╣\n";
     PrintBoxLine("");
     PrintBoxLine("  COLLECTIONS");
     PrintBoxLine("  • List all collections:");
     PrintBoxLine("    " + program_name + " cols");
     PrintBoxLine("");
     PrintBoxLine("  • List with pagination (offset 10, limit 20):");
     PrintBoxLine("    " + program_name + " cols 10 20");
     PrintBoxLine("");
     PrintBoxLine("  • Show collection info:");
     PrintBoxLine("    " + program_name + " info products");
     PrintBoxLine("");
     PrintBoxLine("  • Create collection:");
     PrintBoxLine("    " + program_name + " create products title,content,price");
     PrintBoxLine("");
     PrintBoxLine("  • Delete collection:");
     PrintBoxLine("    " + program_name + " delete products");
     PrintBoxLine("");
     PrintBoxLine("  • Migrate collection into a new name:");
     PrintBoxLine("    " + program_name + " migrate products products_v2");
     PrintBoxLine("");
     PrintBoxLine("  • Delete document:");
     PrintBoxLine("    " + program_name + " delete products 123");
     PrintBoxLine("");
     PrintBoxLine("  • Delete documents matching filter:");
     PrintBoxLine("    " + program_name + " delete products --filter=\"category:electronics\"");
     PrintBoxLine("");
     PrintBoxLine("  DOCUMENTS");
     PrintBoxLine("  • List documents:");
     PrintBoxLine("    " + program_name + " docs products");
     PrintBoxLine("");
     PrintBoxLine("  • List with pagination:");
     PrintBoxLine("    " + program_name + " docs products 0 100");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - BASIC");
     PrintBoxLine("  • Simple search:");
     PrintBoxLine("    " + program_name + " search products \"laptop\"");
     PrintBoxLine("");
     PrintBoxLine("  • Multi-word search:");
     PrintBoxLine("    " + program_name + " search products \"fast laptop\"");
     PrintBoxLine("");
     PrintBoxLine("  • Search with limit (max results):");
     PrintBoxLine("    " + program_name + " search products \"gaming\" 50");
     PrintBoxLine("");
     PrintBoxLine("  • Emit JSON search results:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --json");
     PrintBoxLine("");
     PrintBoxLine("  • Search with limit and offset (pagination):");
     PrintBoxLine("    " + program_name + " search products \"laptop\" 20 10");
     PrintBoxLine("    (limit=20, offset=10)");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - SORTING");
     PrintBoxLine("  • Sort by field ascending:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" 20 0 \"price:asc\"");
     PrintBoxLine("");
     PrintBoxLine("  • Sort by field descending:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" 20 0 \"price:desc\"");
     PrintBoxLine("");
     PrintBoxLine("  • Sort using --sort-by flag:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --sort-by=\"price:asc\"");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - EXACT MATCH");
     PrintBoxLine("  • Prioritize exact matches:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --exact");
     PrintBoxLine("    " + program_name + " search products \"laptop\" -e");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - HIGHLIGHTING");
     PrintBoxLine("  • Enable highlighting in results:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --highlight");
     PrintBoxLine("    " + program_name + " search products \"laptop\" -h");
     PrintBoxLine("");
     PrintBoxLine("  • Highlight specific fields:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --fields=\"title,content\"");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - COMBINED EXAMPLES");
     PrintBoxLine("  • Search with limit, sort, and highlight:");
     PrintBoxLine("    " + program_name + " search products \"gaming\" 50 0 \"price:asc\" --highlight");
     PrintBoxLine("");
     PrintBoxLine("  • Search with exact match and highlighting:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" --exact --highlight");
     PrintBoxLine("");
     PrintBoxLine("  • Complex search with all options:");
     PrintBoxLine("    " + program_name + " search products \"laptop\" 30 10 \"price:desc\" --exact -h");
     PrintBoxLine("");
     PrintBoxLine("  SEARCH - VECTOR SEARCH");
     PrintBoxLine("  • Vector similarity search:");
     PrintBoxLine("    " + program_name + " vector-search products \"0.1,0.2,0.3,...\" embedding 10");
     PrintBoxLine("    " + program_name + " vector-search products \"0.1,0.2,0.3,...\" embedding 10 --json");
     PrintBoxLine("    (comma-separated vector values)");
     PrintBoxLine("");
     PrintBoxLine("  SYNONYMS");
     PrintBoxLine("  • List synonyms:");
     PrintBoxLine("    " + program_name + " synonyms products");
     PrintBoxLine("    " + program_name + " global syn list");
     PrintBoxLine("");
     PrintBoxLine("  • Add synonym group:");
     PrintBoxLine("    " + program_name + " addsyn products laptop \"notebook,computer\"");
     PrintBoxLine("    " + program_name + " global syn add laptop root \"notebook,computer\"");
     PrintBoxLine("");
     PrintBoxLine("  STOPWORDS");
     PrintBoxLine("  • List stopwords:");
     PrintBoxLine("    " + program_name + " stopwords products");
     PrintBoxLine("    " + program_name + " global stop list");
     PrintBoxLine("");
     PrintBoxLine("  • Add stopword:");
     PrintBoxLine("    " + program_name + " addstop products \"the\"");
     PrintBoxLine("    " + program_name + " global stop add \"the\"");
     PrintBoxLine("");
     PrintBoxLine("  STATS");
     PrintBoxLine("  • Show server stats:");
     PrintBoxLine("    " + program_name + " stats");
     PrintBoxLine("");
     PrintBoxLine("  • Show database size:");
     PrintBoxLine("    " + program_name + " dbsize mb");
     PrintBoxLine("");
     PrintBoxLine("  LINKS");
     PrintBoxLine("  • List configured distributed links:");
     PrintBoxLine("    " + program_name + " links");
     PrintBoxLine("");
     PrintBoxLine("  • Ping/check all links:");
     PrintBoxLine("    " + program_name + " links ping");
     PrintBoxLine("");
     PrintBoxLine("  TOPICS");
     PrintBoxLine("  • Ask for natural answer from collection context:");
     PrintBoxLine("    " + program_name + " ask books \"what is this collection about?\" 3 1");
     PrintBoxLine("");
     PrintBoxLine("  • Ask using one specific document:");
     PrintBoxLine("  API KEYS");
     PrintBoxLine("  • Create key for specific collection:");
     PrintBoxLine("    " + program_name + " keys create --cols=products --actions=search --desc=\"Public\"");
     PrintBoxLine("");
     PrintBoxLine("  • Create key with multiple collections and full access:");
     PrintBoxLine("    " + program_name + " keys create --cols=col1,col2 --actions=*");
     PrintBoxLine("");
     PrintBoxLine("  • Update existing key permissions:");
     PrintBoxLine("    " + program_name + " keys update <id> --actions=search,create");
     PrintBoxLine("");
     std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
     std::cout << "\n";
}

/* Shows all available API routes. */

void HLQueryCLI::ShowRoutes()
{
     std::cout << "\nHLQUERY API - ALL AVAILABLE ROUTES\n";
     std::cout << "\n\n";

     std::cout << "HEALTH & INFO:.\n";
     std::cout << "  GET  http://localhost:9200/\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET  http://localhost:9200/health\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET  http://localhost:9200/stats\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET  http://localhost:9200/status\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET  http://localhost:9200/metrics\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "COLLECTIONS:.\n";
     std::cout << "  GET    http://localhost:9200/collections\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      name (string, required): Collection name (1-64 chars, alphanumeric, _, -)\n";
     std::cout << "      fields (array, optional): Array of field objects with name and type\n";
     std::cout << "        - name (string, required): Field name\n";
     std::cout << "        - type (string, optional): Field type (string, int, float, etc.)\n";
     std::cout << "      searchable_fields (array, required if fields not provided): Array of field names\n";
     std::cout << "      filterable_fields (array, optional): Array of filterable field names\n";
     std::cout << "      sortable_fields (array, optional): Array of sortable field names\n";
     std::cout << "      vector_fields (array, optional): Array of vector field names\n";
     std::cout << "      enable_typos (boolean, optional): Enable typo tolerance (default: true)\n";
     std::cout << "      max_typos (integer, optional): Maximum typos allowed (default: 2)\n";
     std::cout << "      enable_synonyms (boolean, optional): Enable synonyms (default: true)\n";
     std::cout << "      language (string, optional): Language code (default: \"en\")\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/update\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required): Same as POST /collections\n\n";

     std::cout << "DOCUMENTS:.\n";
     std::cout << "  GET    http://localhost:9200/collections/{name}/documents\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params:\n";
     std::cout << "      offset (integer, optional): Number of documents to skip (default: 0)\n";
     std::cout << "      limit (integer, optional): Maximum documents to return (default: 10000, max: 100000)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/documents\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required): Document object\n";
     std::cout << "      id (string, required): Document ID (max 1000 chars, no spaces/slashes)\n";
     std::cout << "      [field_name] (any, optional): Any field defined in collection schema\n";
     std::cout << "      Note: Max document size is 20MB\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/documents/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Document ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  PUT    http://localhost:9200/collections/{name}/documents/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Document ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required): Document object (same as POST, id is taken from path)\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}/documents/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Document ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}/documents\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params:\n";
     std::cout << "      filter_by (string, optional): Filter condition (e.g., \"price:>=19\")\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/documents/import\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      documents (array, required): Array of document objects\n\n";

     std::cout << "SEARCH:.\n";
     std::cout << "  GET    http://localhost:9200/collections/{name}/documents/search\n";
     std::cout << "  POST   http://localhost:9200/collections/{name}/documents/search\n";
     std::cout << "  GET    http://localhost:9200/collections/{name}/search\n";
     std::cout << "  POST   http://localhost:9200/collections/{name}/search\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params (GET) / Body (POST, JSON):\n";
     std::cout << "      q (string, required*): Search query string (*required if no vector_query)\n";
     std::cout << "      query_by (string, required*): Comma-separated fields to search (*required if using q)\n";
     std::cout << "      filter_by (string, optional): Filter conditions (e.g., \"price:>=100\")\n";
     std::cout << "      sort_by (string, optional): Sort fields (e.g., \"price:asc,title:desc\")\n";
     std::cout << "      facet_by (string, optional): Comma-separated fields for faceting\n";
     std::cout << "      per_page (integer, optional): Results per page (default: 10, max: 1000)\n";
     std::cout << "      page (integer, optional): Page number (default: 1)\n";
     std::cout << "      limit (integer, optional): Alternative to per_page\n";
     std::cout << "      offset (integer, optional): Alternative to page (number to skip)\n";
     std::cout << "      page_token (string, optional): Token for pagination\n";
     std::cout << "      highlight (boolean, optional): Enable highlighting (default: false)\n";
     std::cout << "      highlight_fields (string, optional): Comma-separated fields to highlight\n";
     std::cout << "      highlight_full_fields (string, optional): Fields to highlight fully\n";
     std::cout << "      include_fields (string, optional): Comma-separated fields to include\n";
     std::cout << "      exclude_fields (string, optional): Comma-separated fields to exclude\n";
     std::cout << "      num_typos (integer, optional): Max typos allowed (default: 2, max: 10)\n";
     std::cout << "      drop_tokens_threshold (integer, optional): Drop tokens threshold (default: 0, disabled unless set)\n";
     std::cout << "      typo_tokens_threshold (integer, optional): Typo tokens threshold (default: 2)\n";
     std::cout << "      prefix (boolean, optional): Enable prefix matching (default: false)\n";
     std::cout << "      group_by (string, optional): Comma-separated fields for grouping\n";
     std::cout << "      group_limit (integer, optional): Results per group (default: 3)\n";
     std::cout << "      prioritize_exact_match (boolean, optional): Prioritize exact matches (default: true)\n";
     std::cout << "      exhaustive_search (boolean, optional): Enable exhaustive search (default: false)\n";
     std::cout << "      vector_query (string, optional): Vector query JSON string\n";
     std::cout << "      embedding (string, optional): Comma-separated vector values\n";
     std::cout << "      use_remote_embedding (boolean, optional): Use remote embedding service\n";
     std::cout << "      remote_embedding_url (string, optional): Remote embedding service URL\n";
     std::cout << "      remote_embedding_api_key (string, optional): API key for remote embedding\n";
     std::cout << "      remote_embedding_model (string, optional): Model name for remote embedding\n";
     std::cout << "      remote_embedding_embedding_field (string, optional): Field name for embedding\n";
     std::cout << "      hybrid_alpha (float, optional): Hybrid search alpha (0.0-1.0, default: 0.5)\n";
     std::cout << "      geo_radius (string, optional): Geo radius \"lat,lon,radius_km\"\n";
     std::cout << "      enable_overrides (boolean, optional): Enable search overrides (default: false)\n";
     std::cout << "      enable_synonyms (boolean, optional): Enable synonyms (default: true)\n";
     std::cout << "      enable_stopwords (boolean, optional): Enable stopwords (default: true)\n";
     std::cout << "      analytics_tag (string, optional): Tag for analytics\n";
     std::cout << "      enable_analytics (boolean, optional): Enable analytics (default: true)\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/vector_search\n";
     std::cout << "  POST   http://localhost:9200/collections/{name}/vector_search\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params (GET) / Body (POST, JSON):\n";
     std::cout << "      vector_query (string/array, required*): Vector as JSON array string or comma-separated values\n";
     std::cout << "        Example: \"[0.1, 0.2, 0.3]\" or \"0.1,0.2,0.3\"\n";
     std::cout << "      embedding (string/array, required*): Alternative to vector_query, same format\n";
     std::cout << "        *Either vector_query or embedding is required\n";
     std::cout << "      field_name (string, optional): Vector field name to search (default: \"embedding\")\n";
     std::cout << "      limit (integer, optional): Maximum results to return (default: 10, max: 10000)\n";
     std::cout << "      threshold (float, optional): Minimum similarity score (default: 0.0, range: -1.0 to 1.0)\n";
     std::cout << "      normalize (boolean, optional): Normalize vectors for cosine similarity (default: true)\n";
     std::cout << "      filter_by (string, optional): Filter conditions (e.g., \"price:>=100\")\n";
     std::cout << "      sort_by (string, optional): Sort fields (e.g., \"price:asc\")\n";
     std::cout << "      include_fields (string, optional): Comma-separated fields to include\n";
     std::cout << "      exclude_fields (string, optional): Comma-separated fields to exclude\n";
     std::cout << "      per_page (integer, optional): Results per page (default: 10, max: 1000)\n";
     std::cout << "      page (integer, optional): Page number (default: 1)\n";
     std::cout << "      offset (integer, optional): Number of results to skip\n";
     std::cout << "      Note: All search parameters from above are also supported\n\n";

     std::cout << "  GET    http://localhost:9200/multi_search\n";
     std::cout << "  POST   http://localhost:9200/multi_search\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params (GET) / Body (POST, JSON):\n";
     std::cout << "      searches (array, required): Array of search query objects\n";
     std::cout << "        Each search object contains:\n";
     std::cout << "          collection (string, required): Collection name\n";
     std::cout << "          q (string, optional): Search query\n";
     std::cout << "          query_by (string, optional): Fields to search\n";
     std::cout << "          [all other search parameters as above]\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/documents/facet_counts\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required): Search parameters (same as search endpoint)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/documents/export\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, optional): Search parameters to filter exported documents\n\n";

     std::cout << "SYNONYMS:.\n";
     std::cout << "  GET    http://localhost:9200/synonyms\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET    http://localhost:9200/synonyms/global\n";
     std::cout << "  POST   http://localhost:9200/synonyms/global/{id}\n";
     std::cout << "  DELETE http://localhost:9200/synonyms/global/{id}\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/synonyms\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/synonyms/{id}\n";
     std::cout << "  PUT    http://localhost:9200/collections/{name}/synonyms/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Synonym ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      root (string, required): Root word\n";
     std::cout << "      synonyms (array, required): Array of synonym strings\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/synonyms/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Synonym ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}/synonyms/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Synonym ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "STOPWORDS:.\n";
     std::cout << "  GET    http://localhost:9200/stopwords\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  GET    http://localhost:9200/stopwords/global\n";
     std::cout << "  POST   http://localhost:9200/stopwords/global\n";
     std::cout << "  DELETE http://localhost:9200/stopwords/global/{word}\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/stopwords\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/stopwords\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      word (string, required): Stopword to add\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}/stopwords/{word}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      word (string, required): Stopword to delete\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "OVERRIDES:.\n";
     std::cout << "  GET    http://localhost:9200/collections/{name}/overrides\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/collections/{name}/overrides/{id}\n";
     std::cout << "  PUT    http://localhost:9200/collections/{name}/overrides/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Override ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      rule (object, required): Override rule\n";
     std::cout << "        query (string, required): Query to match\n";
     std::cout << "        match (string, optional): Match type (exact, contains, etc.)\n";
     std::cout << "      filter_by (string, optional): Filter condition\n";
     std::cout << "      sort_by (string, optional): Sort fields\n";
     std::cout << "      pin_documents (array, optional): Array of document IDs to pin\n";
     std::cout << "      boost_documents (object, optional): Map of document IDs to boost scores\n";
     std::cout << "      demote_documents (object, optional): Map of document IDs to demote scores\n\n";

     std::cout << "  GET    http://localhost:9200/collections/{name}/overrides/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Override ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  DELETE http://localhost:9200/collections/{name}/overrides/{id}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Collection name\n";
     std::cout << "      id (string, required): Override ID\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "ALIASES:.\n";
     std::cout << "  GET    http://localhost:9200/aliases\n";
     std::cout << "    Path params: (none)\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  POST   http://localhost:9200/aliases/{name}\n";
     std::cout << "  PUT    http://localhost:9200/aliases/{name}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Alias name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body (JSON, required):\n";
     std::cout << "      collection_name (string, required): Collection name to alias\n";
     std::cout << "      [other alias configuration fields]\n\n";

     std::cout << "  GET    http://localhost:9200/aliases/{name}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Alias name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "  DELETE http://localhost:9200/aliases/{name}\n";
     std::cout << "    Path params:\n";
     std::cout << "      name (string, required): Alias name\n";
     std::cout << "    Query params: (none)\n";
     std::cout << "    Body: (none)\n\n";

     std::cout << "NOTES:.\n";
     std::cout << "  - Replace {name}, {id}, {word} with actual values\n";
     std::cout << "  - Default port: 9200 (use -url to change)\n";
     std::cout << "  - For GET requests, parameters go in query string\n";
     std::cout << "  - For POST/PUT requests, parameters go in JSON body\n";
     std::cout << "  - Required parameters are marked as 'required'\n";
     std::cout << "  - Optional parameters have default values shown\n\n";

     std::cout << "CURL EXAMPLES:.\n";
     std::cout << "\n\n";

     std::cout << "GET Example - Get a document:\n";
     std::cout << "  curl -X GET http://localhost:9200/collections/mycollection/documents/doc123\n\n";

     std::cout << "POST Example - Create a document:\n";
     std::cout << "  curl -X POST http://localhost:9200/collections/mycollection/documents \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\"id\":\"doc123\",\"title\":\"Example Document\",\"content\":\"This is an example\"}'\n\n";

     std::cout << "PUT Example - Update a document:\n";
     std::cout << "  curl -X PUT http://localhost:9200/collections/mycollection/documents/doc123 \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\"title\":\"Updated Document\",\"content\":\"Updated content\"}'\n\n";

     std::cout << "DELETE Example - Delete a document:\n";
     std::cout << "  curl -X DELETE http://localhost:9200/collections/mycollection/documents/doc123\n\n";

     std::cout << "GET Example - Search with parameters:\n";
     std::cout << "  curl -X GET \"http://localhost:9200/collections/mycollection/search?q=example&query_by=title,content&limit=10&offset=0&filter_by=price:>=100&sort_by=price:asc&facet_by=category&highlight=true\"\n\n";

     std::cout << "POST Example - Search with ALL parameters:\n";
     std::cout << "  curl -X POST http://localhost:9200/collections/mycollection/search \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\n";
     std::cout << "      \"q\": \"example search query\",\n";
     std::cout << "      \"query_by\": \"title,content,description\",\n";
     std::cout << "      \"filter_by\": \"price:>=100 && category:=electronics\",\n";
     std::cout << "      \"sort_by\": \"price:asc,rating:desc\",\n";
     std::cout << "      \"facet_by\": \"category,brand\",\n";
     std::cout << "      \"limit\": 20,\n";
     std::cout << "      \"offset\": 0,\n";
     std::cout << "      \"per_page\": 20,\n";
     std::cout << "      \"page\": 1,\n";
     std::cout << "      \"highlight\": true,\n";
     std::cout << "      \"highlight_fields\": \"title,content\",\n";
     std::cout << "      \"include_fields\": \"id,title,price\",\n";
     std::cout << "      \"exclude_fields\": \"internal_notes\",\n";
     std::cout << "      \"num_typos\": 2,\n";
     std::cout << "      \"prefix\": false,\n";
     std::cout << "      \"prioritize_exact_match\": true,\n";
     std::cout << "      \"exhaustive_search\": false\n";
     std::cout << "    }'\n\n";

     std::cout << "POST Example - Vector search:\n";
     std::cout << "  curl -X POST http://localhost:9200/collections/mycollection/vector_search \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\n";
     std::cout << "      \"vector_query\": [0.1, 0.2, 0.3, 0.4, 0.5],\n";
     std::cout << "      \"field_name\": \"embedding\",\n";
     std::cout << "      \"limit\": 10,\n";
     std::cout << "      \"threshold\": 0.7,\n";
     std::cout << "      \"normalize\": true,\n";
     std::cout << "      \"filter_by\": \"category:=electronics\",\n";
     std::cout << "      \"include_fields\": \"id,title,score\"\n";
     std::cout << "    }'\n\n";

     std::cout << "GET Example - Vector search (alternative format):\n";
     std::cout << "  curl -X GET \"http://localhost:9200/collections/mycollection/vector_search?embedding=0.1,0.2,0.3,0.4,0.5&limit=10&threshold=0.7&field_name=embedding\"\n\n";

     std::cout << "POST Example - Multi-search:\n";
     std::cout << "  curl -X POST http://localhost:9200/multi_search \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\n";
     std::cout << "      \"searches\": [\n";
     std::cout << "        {\n";
     std::cout << "          \"collection\": \"products\",\n";
     std::cout << "          \"q\": \"laptop\",\n";
     std::cout << "          \"query_by\": \"title,description\",\n";
     std::cout << "          \"limit\": 5\n";
     std::cout << "        },\n";
     std::cout << "        {\n";
     std::cout << "          \"collection\": \"reviews\",\n";
     std::cout << "          \"q\": \"excellent\",\n";
     std::cout << "          \"query_by\": \"content\",\n";
     std::cout << "          \"limit\": 10\n";
     std::cout << "        }\n";
     std::cout << "      ]\n";
     std::cout << "    }'\n\n";

     std::cout << "GET Example - List documents with pagination:\n";
     std::cout << "  curl -X GET \"http://localhost:9200/collections/mycollection/documents?offset=0&limit=100\"\n\n";

     std::cout << "POST Example - Create collection:\n";
     std::cout << "  curl -X POST http://localhost:9200/collections \\\n";
     std::cout << "    -H \"Content-Type: application/json\" \\\n";
     std::cout << "    -d '{\n";
     std::cout << "      \"name\": \"products\",\n";
     std::cout << "      \"searchable_fields\": [\"title\", \"description\", \"content\"],\n";
     std::cout << "      \"filterable_fields\": [\"price\", \"category\", \"brand\"],\n";
     std::cout << "      \"sortable_fields\": [\"price\", \"rating\", \"created_at\"],\n";
     std::cout << "      \"vector_fields\": [\"embedding\"],\n";
     std::cout << "      \"enable_typos\": true,\n";
     std::cout << "      \"max_typos\": 2\n";
     std::cout << "    }'\n\n";
}

/* Shows help information. */

void HLQueryCLI::ShowHelp()
{
     std::cout << ProgramName << " v1.0.0.\n";
     std::cout << "Command Line Interface for hlquery search engine.\n\n";

     std::cout << "USAGE:.\n";
     std::cout << "    " << ProgramName << " [OPTIONS] <COMMAND> [ARGS...].\n\n";

     std::cout << "OPTIONS:.\n";
     std::cout << "    -url, --url URL          Server URL (default: http://localhost:9200, env: HLQ_BASE_URL).\n";
     std::cout << "    --host HOST             Server hostname or IP (default: localhost).\n";
     std::cout << "    --port PORT             Server port (default: 9200).\n";
     std::cout << "    -auth, --auth TOKEN     Authentication token (env: HLQ_AUTH_TOKEN).\n";
     std::cout << "    --ssl-auth              Over HTTPS, send token as both Authorization and X-API-Key (env: HLQ_SSL_AUTH).\n";
     std::cout << "    --timeout SECONDS       Request timeout in seconds (default: 30).\n";
     std::cout << "    --raw                   Print raw HTTP JSON responses.\n";
     std::cout << "    -h, --help              Show this help message.\n\n";
     std::cout << "    Note: --host and --port can be used together instead of -url.\n";
     std::cout << "          Example: --host localhost --port 9201.\n\n";

     std::cout << "HEALTH/STATUS:.\n";
     std::cout << "    health                  Quick health check (/health and /stats).\n";
     std::cout << "    stats                   Show server statistics.\n";
     std::cout << "    links [ping]            Show distributed links (use 'ping' to fail on unreachable links).\n";
     std::cout << "    ask [collection] \"q\" [max] [s]  Return natural-language answer (auto-detects collection intent).\n";
     std::cout << "    adv                     Show advanced detailed server information.\n";
     std::cout << "    uptime [-d]             Show server uptime (-d includes seconds).\n\n";

     std::cout << "COLLECTIONS:.\n";
     std::cout << "    cols [--json] [--offset N] [--limit N]  List collections with consistency checks.\n";
     std::cout << "    show cols [--offset N] [--limit N]  Alias for cols; list all collections.\n";
     std::cout << "    info <collection>       Show detailed information about a collection.\n";
     std::cout << "    doctotal [--offset N] [--limit N]  Show total docs/cols (cross-checks endpoints).\n";
     std::cout << "    coltotal                Alias for doctotal.\n";
     std::cout << "";
     std::cout << "    rebuild-counters [collection] [--index]  Rebuild doc counters from source of truth (LSM + WAL) and compare.\n";
     std::cout << "    repair [collection] [--index]  Full repair: rebuild counters and optionally inverted index (alias for rebuild-counters --index).\n";
     std::cout << "    verify                  Run full consistency suite (doctotal, counts, sampling).\n\n";

     std::cout << "DOCUMENTS:.\n";
     std::cout << "    docs <collection> [offset] [limit]  List documents (default: 10k).\n";
     std::cout << "    search <col> <query> [limit] [offset] [sort] [--all] [--exact] [--highlight] [--fields=field1,field2] [--maybe=min,limit] [--json].\n";
     std::cout << "    search --all <query> [limit] [offset] [sort] [--exact] [--highlight] [--collections=col1,col2] [--fields=field1,field2] [--maybe=min,limit] [--json].\n";
     std::cout << "          Limit/offset/sort control pagination, --exact prioritizes exact matches, --highlight prints snippets, --fields shows specific columns, and --maybe adds suggestions when results are below min.\n";
     std::cout << "          Without --all, search stays local to the selected server. Use --all to fan out across cluster search nodes only; slaves configured for replication are not query targets here.\n";
     std::cout << "    vector-search <col> <vector> [field] [limit] [--json].\n";
     std::cout << "          Run vector similarity search against one collection (default field: embedding).\n";
     std::cout << "    maybe <query> [collection] [limit] [min_results] [--json].\n";
     std::cout << "          Return up to 5 suggestions similar to your wording; if no local hits, it checks two other collections as best-guess fallback.\n";
     std::cout << "    colsearch <query> [limit] [offset] [sort] [--all] [--maybe=min,limit] [--json].\n";
     std::cout << "          Without --all, colsearch stays local to the selected server.\n";
     std::cout << "          Search collection names by exact name, or use * / ? wildcard patterns when enabled in config.\n";
     std::cout << "    open <col>/<doc> [--table|--json|--text]  Open and display a specific document.\n";
     std::cout << "    select <field> <col>/<doc>  Select and display a specific field value.\n\n";
     std::cout << "    update <field> <col>/<doc> <value>  Partially update one field in a document.\n\n";
     std::cout << "    migrate <source> <target> [--drop-old]  Create a new collection and copy documents into it.\n\n";

     std::cout << "LSM/ENGINE:.\n";
     std::cout << "    lsm                     Show deep RocksDB engine statistics.\n";
     std::cout << "    dbsize [mb|gb|kb]       Show database size (default: MB).\n";
     std::cout << "    tx [kb|mb|gb]           Show total bytes transferred (default: KB).\n\n";

     std::cout << "MODULES:.\n";
     std::cout << "    modules [1|0]           List loaded modules, core only with 1, optional only with 0.\n";
     std::cout << "    module info             List loaded modules with routes.\n";
     std::cout << "    module <name> info      Show module routes and parameter info.\n";
     std::cout << "    module <name> syntax    Show module command syntax.\n";
     std::cout << "    module <name> <route> [--json] [--key=value] [args...]  Run one module command.\n";
     std::cout << "    loadmodule <name>       Load one runtime module.\n";
     std::cout << "    unloadmodule <name>     Unload one runtime module.\n\n";

     std::cout << "DANGEROUS OPS:.\n";
     std::cout << "    create <name> [fields]  Create a new collection.\n";
     std::cout << "    delete <collection> [id] [--filter=q]  Delete a collection, a document, or by filter.\n";
     std::cout << "    delete <collection>/<id>              Delete a document using path-style syntax.\n";
     std::cout << "    flush [--force]         Delete ALL collections and data from the system.\n";
     std::cout << "    add <col> <id> <title> <content> [field=value...]  Add document.\n\n";

     std::cout << "API KEYS:.\n";
     std::cout << "    keys list               List all API keys.\n";
     std::cout << "    keys create [--desc=...] [--cols=c1,c2] [--actions=a1,a2]  Create a new API key.\n";
     std::cout << "    keys delete <id>        Delete an API key.\n";
     std::cout << "    keys update <id> [options] Update an existing API key.\n\n";

     std::cout << "DEBUG:.\n";
     std::cout << "    debug benchmark-state   Inspect benchmark collections and counter consistency.\n\n";

     std::cout << "EXIT CODES:.\n";
     std::cout << "    0  Success.\n";
     std::cout << "    1  Bad arguments or unknown command.\n";
     std::cout << "    2  Server error (connection failed, HTTP error, etc.).\n";
     std::cout << "    3  Data inconsistency detected.\n\n";
}
