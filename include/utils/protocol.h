/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#pragma once

#include <string>

#include "core/httpcodes.h"

/* Enumerated HTTP status codes. */

enum Status
{
     /* 2xx Success. */

     OK 		  =   200,
     CREATED 	 	  =   201,
     ACCEPTED 	          =   202,
     NO_CONTENT           =   204,
     MULTI_STATUS         =   207,

     /* 3xx Redirection. */

     MOVED_PERMANENTLY    =   301,
     FOUND                =   302,
     NOT_MODIFIED         =   304,

     /* 4xx Client errors. */

     BAD_REQUEST          =   400,
     UNAUTHORIZED 	  =   401,
     FORBIDDEN 	          =   403,
     NOT_FOUND 		  =   404,
     METHOD_NOT_ALLOWED   =   405,
     CONFLICT 	          =   409,
     PAYLOAD_TOO_LARGE    =   413,
     UNPROCESSABLE_ENTITY =   422,
     TOO_MANY_REQUESTS    =   429,

     /* 5xx Server errors. */

     INTERNAL_SERVER_ERROR =  500,
     NOT_IMPLEMENTED       =  501,
     BAD_GATEWAY           =  502,
     SERVICE_UNAVAILABLE   =  503,
     GATEWAY_TIMEOUT       =  504
};

/* Status text mapping removed. Use StatusText from core/httpcodes.h. */

/* Application-level protocol codes for internal communication. */

/* Enumerated application protocol codes. */

enum Code
{
     /* Success codes (20000-20999). */

     SUCCESS = 20000,
     OPERATION_COMPLETE = 20001,

     /* Collection codes (21000-21999). */

     COLLECTION_NOT_FOUND = 21000,
     COLLECTION_EMPTY = 21001,
     COLLECTION_EXISTS = 21002,
     COLLECTION_INVALID_NAME = 21003,
     COLLECTION_INVALID_SCHEMA = 21004,
     COLLECTION_CREATED = 21005,
     COLLECTION_UPDATED = 21006,
     COLLECTION_DELETED = 21007,

     /* Document codes (22000-22999). */

     DOCUMENT_NOT_FOUND = 22000,
     DOCUMENT_INVALID_ID = 22001,
     DOCUMENT_INVALID_FORMAT = 22002,
     DOCUMENT_CREATED = 22003,
     DOCUMENT_UPDATED = 22004,
     DOCUMENT_DELETED = 22005,
     DOCUMENT_BULK_IMPORTED = 22006,

     /* Search codes (23000-23999). */

     SEARCH_INVALID_QUERY = 23000,
     SEARCH_INVALID_PARAMETER = 23001,
     SEARCH_NO_RESULTS = 23002,
     SEARCH_SUCCESS = 23003,
     SEARCH_EMPTY_QUERY = 23004,
     SEARCH_INVALID_FIELD = 23005,

     /* Validation codes (24000-24999). */

     VALIDATION_FAILED = 24000,
     VALIDATION_INVALID_JSON = 24001,
     VALIDATION_MISSING_FIELD = 24002,
     VALIDATION_INVALID_TYPE = 24003,
     VALIDATION_INVALID_VALUE = 24004,

     /* Authentication codes (25000-25999). */

     AUTH_REQUIRED = 25000,
     AUTH_INVALID = 25001,
     AUTH_EXPIRED = 25002,
     AUTH_FORBIDDEN = 25003,

     /* System codes (26000-26999). */

     SYSTEM_ERROR = 26000,
     SYSTEM_UNAVAILABLE = 26001,
     SYSTEM_SYNCING = 26002,
     SYSTEM_SHUTTING_DOWN = 26003,
     SYSTEM_MAINTENANCE = 26004,

     /* Storage codes (27000-27999). */

     STORAGE_ERROR = 27000,
     STORAGE_FULL = 27001,
     STORAGE_IO_ERROR = 27002,
     STORAGE_LOCKED = 27003,

     /* Synonym codes (28000-28999). */

     SYNONYM_NOT_FOUND = 28000,
     SYNONYM_EXISTS = 28001,
     SYNONYM_INVALID = 28002,
     SYNONYM_CREATED = 28003,
     SYNONYM_UPDATED = 28004,
     SYNONYM_DELETED = 28005,

     /* Stopword codes (29000-29999). */

     STOPWORD_NOT_FOUND = 29000,
     STOPWORD_EXISTS = 29001,
     STOPWORD_INVALID = 29002,
     STOPWORD_CREATED = 29003,
     STOPWORD_DELETED = 29004,

     /* Override codes (30000-30999). */

     OVERRIDE_NOT_FOUND = 30000,
     OVERRIDE_EXISTS = 30001,
     OVERRIDE_INVALID = 30002,
     OVERRIDE_CREATED = 30003,
     OVERRIDE_UPDATED = 30004,
     OVERRIDE_DELETED = 30005,

     /* Alias codes (31000-31999). */

     ALIAS_NOT_FOUND = 31000,
     ALIAS_EXISTS = 31001,
     ALIAS_INVALID = 31002,
     ALIAS_CREATED = 31003,
     ALIAS_UPDATED = 31004,
     ALIAS_DELETED = 31005,

     /* Module codes (32000-32999). */

     MODULE_NOT_FOUND = 32000,
     MODULE_ROUTE_NOT_FOUND = 32001,
     MODULE_UNAVAILABLE = 32002,

     /* Rate limiting codes (33000-33999). */
     RATE_LIMIT_EXCEEDED = 33000
};

/* Map a protocol code to its text description. */

inline const char* CodeText(int CodeValue)
{

     switch (CodeValue)
     {

          case SUCCESS:
               return "Success";

          case OPERATION_COMPLETE:
               return "Operation Complete";

          case COLLECTION_NOT_FOUND:
               return "Collection Not Found";

          case COLLECTION_EMPTY:
               return "Collection Empty";

          case COLLECTION_EXISTS:
               return "Collection Exists";

          case COLLECTION_INVALID_NAME:
               return "Invalid Collection Name";

          case COLLECTION_INVALID_SCHEMA:
               return "Invalid Collection Schema";

          case COLLECTION_CREATED:
               return "Collection Created";

          case COLLECTION_UPDATED:
               return "Collection Updated";

          case COLLECTION_DELETED:
               return "Collection Deleted";

          case DOCUMENT_NOT_FOUND:
               return "Document Not Found";

          case DOCUMENT_INVALID_ID:
               return "Invalid Document ID";

          case DOCUMENT_INVALID_FORMAT:
               return "Invalid Document Format";

          case DOCUMENT_CREATED:
               return "Document Created";

          case DOCUMENT_UPDATED:
               return "Document Updated";

          case DOCUMENT_DELETED:
               return "Document Deleted";

          case DOCUMENT_BULK_IMPORTED:
               return "Documents Bulk Imported";

          case SEARCH_INVALID_QUERY:
               return "Invalid Search Query";

          case SEARCH_INVALID_PARAMETER:
               return "Invalid Search Parameter";

          case SEARCH_NO_RESULTS:
               return "No Search Results";

          case SEARCH_SUCCESS:
               return "Search Success";

          case SEARCH_EMPTY_QUERY:
               return "Empty Search Query";

          case SEARCH_INVALID_FIELD:
               return "Invalid Search Field";

          case VALIDATION_FAILED:
               return "Validation Failed";

          case VALIDATION_INVALID_JSON:
               return "Invalid JSON";

          case VALIDATION_MISSING_FIELD:
               return "Missing Required Field";

          case VALIDATION_INVALID_TYPE:
               return "Invalid Field Type";

          case VALIDATION_INVALID_VALUE:
               return "Invalid Field Value";

          case AUTH_REQUIRED:
               return "Authentication Required";

          case AUTH_INVALID:
               return "Invalid Authentication";

          case AUTH_EXPIRED:
               return "Authentication Expired";

          case AUTH_FORBIDDEN:
               return "Forbidden";

          case SYSTEM_ERROR:
               return "System Error";

          case SYSTEM_UNAVAILABLE:
               return "System Unavailable";

          case SYSTEM_SYNCING:
               return "System Syncing";

          case SYSTEM_SHUTTING_DOWN:
               return "System Shutting Down";

          case SYSTEM_MAINTENANCE:
               return "System Maintenance";

          case STORAGE_ERROR:
               return "Storage Error";

          case STORAGE_FULL:
               return "Storage Full";

          case STORAGE_IO_ERROR:
               return "Storage I/O Error";

          case STORAGE_LOCKED:
               return "Storage Locked";

          case SYNONYM_NOT_FOUND:
               return "Synonym Not Found";

          case SYNONYM_EXISTS:
               return "Synonym Exists";

          case SYNONYM_INVALID:
               return "Invalid Synonym";

          case SYNONYM_CREATED:
               return "Synonym Created";

          case SYNONYM_UPDATED:
               return "Synonym Updated";

          case SYNONYM_DELETED:
               return "Synonym Deleted";

          case STOPWORD_NOT_FOUND:
               return "Stopword Not Found";

          case STOPWORD_EXISTS:
               return "Stopword Exists";

          case STOPWORD_INVALID:
               return "Invalid Stopword";

          case STOPWORD_CREATED:
               return "Stopword Created";

          case STOPWORD_DELETED:
               return "Stopword Deleted";

          case OVERRIDE_NOT_FOUND:
               return "Override Not Found";

          case OVERRIDE_EXISTS:
               return "Override Exists";

          case OVERRIDE_INVALID:
               return "Invalid Override";

          case OVERRIDE_CREATED:
               return "Override Created";

          case OVERRIDE_UPDATED:
               return "Override Updated";

          case OVERRIDE_DELETED:
               return "Override Deleted";

          case ALIAS_NOT_FOUND:
               return "Alias Not Found";

          case ALIAS_EXISTS:
               return "Alias Exists";

          case ALIAS_INVALID:
               return "Invalid Alias";

          case ALIAS_CREATED:
               return "Alias Created";

          case ALIAS_UPDATED:
               return "Alias Updated";

          case ALIAS_DELETED:
               return "Alias Deleted";

          case MODULE_NOT_FOUND:
               return "Module Not Found";

          case MODULE_ROUTE_NOT_FOUND:
               return "Module Route Not Found";

          case MODULE_UNAVAILABLE:
               return "Module Unavailable";

          case RATE_LIMIT_EXCEEDED:
               return "Rate limit exceeded";

          default:
               return "Unknown Code";
     }
}
