/*
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 * Copyright (C) 2014 Vivocha S.p.A.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "../mainrelay.h"
#include "dbd_mongo.h"

#if !defined(TURN_NO_MONGO)
#include <mongoc.h>
#include <bson.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////

const char * MONGO_DEFAULT_DB = "turn";

struct _MONGO {
  mongoc_uri_t * uri;
  mongoc_client_t * client;
  const char * database;
};

typedef struct _MONGO MONGO;

static void mongo_logger(mongoc_log_level_t log_level, const char * log_domain, const char * message, void * user_data) {
	UNUSED_ARG(log_domain);
	UNUSED_ARG(user_data);

  TURN_LOG_LEVEL l = TURN_LOG_LEVEL_INFO;
  
  UNUSED_ARG(l);

  switch(log_level) {
    case MONGOC_LOG_LEVEL_ERROR:
      l = TURN_LOG_LEVEL_ERROR;
      break;
    case MONGOC_LOG_LEVEL_WARNING:
      l = TURN_LOG_LEVEL_WARNING;
      break;
    default:
      l = TURN_LOG_LEVEL_INFO;
      break;
  }
	TURN_LOG_FUNC(l, "%s\n", message);
}

static void MongoFree(MONGO * info) {
	if(info) {
		if(info->uri) mongoc_uri_destroy(info->uri);
		if(info->client) mongoc_client_destroy(info->client);
		turn_free(info, sizeof(MONGO));
	}
}

static MONGO * get_mongodb_connection(void) {

	persistent_users_db_t * pud = get_persistent_users_db();

	MONGO * mydbconnection = (MONGO *) (pud->connection);

	if (!mydbconnection) {
		mongoc_init();
		mongoc_log_set_handler(&mongo_logger, NULL);

		mydbconnection = (MONGO *) turn_malloc(sizeof(MONGO));
		mydbconnection->uri = mongoc_uri_new(pud->userdb);

		if (!mydbconnection->uri) {
			TURN_LOG_FUNC(
					TURN_LOG_LEVEL_ERROR,
					"Cannot open parse MongoDB URI <%s>, connection string format error\n",
					pud->userdb);
			MongoFree(mydbconnection);
			mydbconnection = NULL;
		} else {
			mydbconnection->client = mongoc_client_new_from_uri(
					mydbconnection->uri);
			if (!mydbconnection->client) {
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
						"Cannot initialize MongoDB connection\n");
				MongoFree(mydbconnection);
				mydbconnection = NULL;
			} else {
				mydbconnection->database = mongoc_uri_get_database(
						mydbconnection->uri);
				if (!mydbconnection->database)
					mydbconnection->database = MONGO_DEFAULT_DB;
				pud->connection = mydbconnection;
				TURN_LOG_FUNC(
					TURN_LOG_LEVEL_INFO,
					"Opened MongoDB URI <%s>\n",
					pud->userdb);
			}
		}
	}
	return mydbconnection;
}

static mongoc_collection_t * mongo_get_collection(const char * name) {
	MONGO * mc = get_mongodb_connection();

	if(!mc) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error getting a connection to MongoDB\n");
		return NULL;
	}
    
  mongoc_collection_t * collection;
  collection = mongoc_client_get_collection(mc->client, mc->database, name);

  if (!collection) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error retrieving MongoDB collection '%s'\n", name);
  }

  return collection;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

static int mongo_get_auth_secrets(secrets_list_t *sl, u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("turn_secret"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "value", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'turn_secret'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    const char * value;
    while(mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "value") && BSON_ITER_HOLDS_UTF8(&iter)) {
        value = bson_iter_utf8(&iter, &length);
				add_to_secrets_list(sl, value);
      }
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }

  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static int mongo_get_user_key(u08bits *usname, u08bits *realm, hmackey_t key) {
  mongoc_collection_t * collection = mongo_get_collection("turnusers_lt"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "name", (const char *)usname);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "hmackey", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0, &query, &fields, NULL);
  
  int ret = -1;

  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'turnusers_lt'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    const char * value;
    if (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "hmackey") && BSON_ITER_HOLDS_UTF8(&iter)) {
        value = bson_iter_utf8(&iter, &length);
				size_t sz = get_hmackey_size(turn_params.shatype) * 2;
				if(length < sz) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong key format: string length=%d (must be %d): user %s\n", (int)length, (int)sz, usname);
				} else {
					char kval[sizeof(hmackey_t) + sizeof(hmackey_t) + 1];
					ns_bcopy(value, kval, sz);
					kval[sz] = 0;
					if(convert_string_key_to_binary(kval, key, sz / 2) < 0) {
						TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong key: %s, user %s\n", kval, usname);
					} else {
						ret = 0;
					}
				}
      }
    }
    mongoc_cursor_destroy(cursor);
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}

static int mongo_get_oauth_key(const u08bits *kid, oauth_key_data_raw *key) {

	mongoc_collection_t * collection = mongo_get_collection("oauth_key");

	if (!collection)
		return -1;

	bson_t query;
	bson_init(&query);
	BSON_APPEND_UTF8(&query, "kid", (const char *)kid);

	bson_t fields;
	bson_init(&fields);
	BSON_APPEND_INT32(&fields, "lifetime", 1);
	BSON_APPEND_INT32(&fields, "timestamp", 1);
	BSON_APPEND_INT32(&fields, "as_rs_alg", 1);
	BSON_APPEND_INT32(&fields, "as_rs_key", 1);
	BSON_APPEND_INT32(&fields, "auth_alg", 1);
	BSON_APPEND_INT32(&fields, "auth_key", 1);
	BSON_APPEND_INT32(&fields, "hkdf_hash_func", 1);
	BSON_APPEND_INT32(&fields, "ikm_key", 1);

	mongoc_cursor_t * cursor;
	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0,
			&query, &fields, NULL);

	int ret = -1;

	ns_bzero(key,sizeof(oauth_key_data_raw));
	STRCPY(key->kid,kid);

	if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
				"Error querying MongoDB collection 'oauth_key'\n");
	} else {
		const bson_t * item;
		uint32_t length;
		bson_iter_t iter;
		if (mongoc_cursor_next(cursor, &item)) {
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "as_rs_alg") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->as_rs_alg,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "as_rs_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->as_rs_key,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "auth_alg") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->auth_alg,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "auth_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->auth_key,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "ikm_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->ikm_key,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "hkdf_hash_func") && BSON_ITER_HOLDS_UTF8(&iter)) {
				STRCPY(key->hkdf_hash_func,bson_iter_utf8(&iter, &length));
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "timestamp") && BSON_ITER_HOLDS_INT64(&iter)) {
				key->timestamp = (u64bits)bson_iter_int64(&iter);
			}
			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "lifetime") && BSON_ITER_HOLDS_INT32(&iter)) {
				key->lifetime = (u32bits)bson_iter_int32(&iter);
			}
			ret = 0;
		}
		mongoc_cursor_destroy(cursor);
	}
	mongoc_collection_destroy(collection);
	bson_destroy(&query);
	bson_destroy(&fields);
	return ret;
}
  
static int mongo_get_user_pwd(u08bits *usname, st_password_t pwd) {
  mongoc_collection_t * collection = mongo_get_collection("turnusers_st"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "name", (const char *)usname);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "password", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0, &query, &fields, NULL);
  
  int ret = -1;

  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'turnusers_st'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    const char * value;
    if (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "password") && BSON_ITER_HOLDS_UTF8(&iter)) {
        value = bson_iter_utf8(&iter, &length);

				if(length < 1) {
					TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Wrong password data for user %s, size in MongoDB is zero(0)\n", usname);
				} else {
					ns_bcopy(value, pwd, length);
					pwd[length] = 0;
					ret = 0;
				}
			}
    }
    mongoc_cursor_destroy(cursor);
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static int mongo_set_user_key(u08bits *usname, u08bits *realm, const char *key) {
  mongoc_collection_t * collection = mongo_get_collection("turnusers_lt"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "name", (const char *)usname);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
      
  bson_t doc;
  bson_init(&doc);
  BSON_APPEND_UTF8(&doc, "name", (const char *)usname);
  BSON_APPEND_UTF8(&doc, "realm", (const char *)realm);
  BSON_APPEND_UTF8(&doc, "hmackey", (const char *)key);

  int ret = -1;
  
  if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating user key information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&doc);
  bson_destroy(&query);
  return ret;
}

static int mongo_set_oauth_key(oauth_key_data_raw *key) {

  mongoc_collection_t * collection = mongo_get_collection("oauth_key");

  if(!collection)
    return -1;

  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "kid", (const char *)key->kid);

  bson_t doc;
  bson_init(&doc);
  BSON_APPEND_UTF8(&doc, "kid", (const char *)key->kid);
  BSON_APPEND_UTF8(&doc, "as_rs_alg", (const char *)key->as_rs_alg);
  BSON_APPEND_UTF8(&doc, "as_rs_key", (const char *)key->as_rs_key);
  BSON_APPEND_UTF8(&doc, "auth_alg", (const char *)key->auth_alg);
  BSON_APPEND_UTF8(&doc, "auth_key", (const char *)key->auth_key);
  BSON_APPEND_UTF8(&doc, "hkdf_hash_func", (const char *)key->hkdf_hash_func);
  BSON_APPEND_UTF8(&doc, "ikm_key", (const char *)key->ikm_key);
  BSON_APPEND_INT64(&doc, "timestamp", (int64_t)key->timestamp);
  BSON_APPEND_INT32(&doc, "lifetime", (int32_t)key->lifetime);

  int ret = -1;

  if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating oauth key information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&doc);
  bson_destroy(&query);
  return ret;
}
  
static int mongo_set_user_pwd(u08bits *usname, st_password_t pwd) {
  mongoc_collection_t * collection = mongo_get_collection("turnusers_st");

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "name", (const char *)usname);
  
  bson_t doc;
  bson_init(&doc);
  BSON_APPEND_UTF8(&doc, "name", (const char *)usname);
  BSON_APPEND_UTF8(&doc, "password", (const char *)pwd);

  int ret = -1;
  
  if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating secret key information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&doc);
  bson_destroy(&query);
  return ret;
}
  
static int mongo_del_user(u08bits *usname, int is_st, u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection(is_st ? "turnusers_st" : "turnusers_lt");

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "name", (const char *)usname);
  if(!is_st) {
    BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
  }
  
  int ret = -1;    

  if (!mongoc_collection_delete(collection, MONGOC_DELETE_SINGLE_REMOVE, &query, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error deleting user key information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  return ret;
}

static int mongo_del_oauth_key(const u08bits *kid) {

  mongoc_collection_t * collection = mongo_get_collection("oauth_key");

  if(!collection)
    return -1;

  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "kid", (const char *)kid);

  int ret = -1;

  if (!mongoc_collection_delete(collection, MONGOC_DELETE_SINGLE_REMOVE, &query, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error deleting oauth key information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  return ret;
}
  
static int mongo_list_users(int is_st, u08bits *realm) {
  const char * collection_name = is_st ? "turnusers_st" : "turnusers_lt";
  mongoc_collection_t * collection = mongo_get_collection(collection_name); 

	if(!collection)
    return -1;
    
  bson_t query, child;
  bson_init(&query);
  bson_append_document_begin(&query, "$orderby", -1, &child);
  bson_append_int32(&child, "name", -1, 1);
  bson_append_document_end(&query, &child);
  bson_append_document_begin(&query, "$query", -1, &child);
  if (!is_st && realm && realm[0]) {
    BSON_APPEND_UTF8(&child, "realm", (const char *)realm);
  }
  bson_append_document_end(&query, &child);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "name", 1);
  if(!is_st) BSON_APPEND_INT32(&fields, "realm", 1);

  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection '%s'\n", collection_name);
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    bson_iter_t iter_realm;
    const char * value;
    while (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "name") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		value = bson_iter_utf8(&iter, &length);
    		if (length) {
        		const char *realm = "";
    			if (!is_st && bson_iter_init(&iter_realm, item) && bson_iter_find(&iter_realm, "realm") && BSON_ITER_HOLDS_UTF8(&iter_realm)) {
    				realm = bson_iter_utf8(&iter_realm, &length);
    			}
    			if(realm && *realm) {
    				printf("%s[%s]\n", value, realm);
    			} else {
    				printf("%s\n", value);
    			}
    		}
    	}
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}

static int mongo_list_oauth_keys(void) {

  const char * collection_name = "oauth_key";
  mongoc_collection_t * collection = mongo_get_collection(collection_name);

  if(!collection)
    return -1;

  bson_t query;
  bson_init(&query);

  bson_t child;
  bson_append_document_begin(&query, "$orderby", -1, &child);
  bson_append_int32(&child, "kid", -1, 1);
  bson_append_document_end(&query, &child);
  bson_append_document_begin(&query, "$query", -1, &child);
  bson_append_document_end(&query, &child);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "kid", 1);
  BSON_APPEND_INT32(&fields, "lifetime", 1);
  BSON_APPEND_INT32(&fields, "timestamp", 1);
  BSON_APPEND_INT32(&fields, "as_rs_alg", 1);
  BSON_APPEND_INT32(&fields, "as_rs_key", 1);
  BSON_APPEND_INT32(&fields, "auth_alg", 1);
  BSON_APPEND_INT32(&fields, "auth_key", 1);
  BSON_APPEND_INT32(&fields, "hkdf_hash_func", 1);
  BSON_APPEND_INT32(&fields, "ikm_key", 1);

  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;

  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection '%s'\n", collection_name);
  } else {
    const bson_t * item;
	oauth_key_data_raw key_;
	oauth_key_data_raw *key=&key_;
    uint32_t length;
    bson_iter_t iter;
    while (mongoc_cursor_next(cursor, &item)) {

    	ns_bzero(key,sizeof(oauth_key_data_raw));
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "kid") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->kid,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "as_rs_alg") && BSON_ITER_HOLDS_UTF8(&iter)) {
    	    STRCPY(key->as_rs_alg,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "as_rs_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->as_rs_key,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "auth_alg") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->auth_alg,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "auth_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->auth_key,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "ikm_key") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->ikm_key,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "hkdf_hash_func") && BSON_ITER_HOLDS_UTF8(&iter)) {
    		STRCPY(key->hkdf_hash_func,bson_iter_utf8(&iter, &length));
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "timestamp") && BSON_ITER_HOLDS_INT64(&iter)) {
    		key->timestamp = (u64bits)bson_iter_int64(&iter);
    	}
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "lifetime") && BSON_ITER_HOLDS_INT32(&iter)) {
    		key->lifetime = (u32bits)bson_iter_int32(&iter);
    	}
    	printf("  kid=%s, ikm_key=%s, timestamp=%llu, lifetime=%lu, hkdf_hash_func=%s, as_rs_alg=%s, as_rs_key=%s, auth_alg=%s, auth_key=%s\n",
    		key->kid, key->ikm_key, (unsigned long long)key->timestamp, (unsigned long)key->lifetime, key->hkdf_hash_func,
    		key->as_rs_alg, key->as_rs_key, key->auth_alg, key->auth_key);
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static int mongo_show_secret(u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("turn_secret"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "value", 1);

  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'turn_secret'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    const char * value;
    while (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "value") && BSON_ITER_HOLDS_UTF8(&iter)) {
        value = bson_iter_utf8(&iter, &length);
        if (length) {
					printf("%s\n", value);
        }
      }
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static int mongo_del_secret(u08bits *secret, u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("turn_secret"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
	if(secret && (secret[0]!=0)) {
    BSON_APPEND_UTF8(&query, "value", (const char *)secret);
	}

  mongoc_collection_delete(collection, MONGOC_DELETE_NONE, &query, NULL, NULL);
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  return 0;
}
  
static int mongo_set_secret(u08bits *secret, u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("turn_secret"); 

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
  BSON_APPEND_UTF8(&query, "value", (const char *)secret);

  int res = mongoc_collection_insert(collection, MONGOC_INSERT_NONE, &query, NULL, NULL);
  mongoc_collection_destroy(collection);
  bson_destroy(&query);

  if (!res) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating secret key information\n");
    return -1;
  } else {
    return 0;
  }
}
  
static int mongo_add_origin(u08bits *origin, u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("realm"); 

	if(!collection)
    return -1;
    
  int ret = -1;
  
  bson_t query, doc, child;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
  bson_init(&doc);
  bson_append_document_begin(&doc, "$addToSet", -1, &child);
  BSON_APPEND_UTF8(&child, "origin", (const char *)origin);
  bson_append_document_end(&doc, &child);

  if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error inserting/updating realm origin information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&doc);
  return ret;
}
  
static int mongo_del_origin(u08bits *origin) {
  mongoc_collection_t * collection = mongo_get_collection("realm"); 

	if(!collection)
    return -1;
    
  int ret = -1;
  
  bson_t query, doc, child;
  bson_init(&query);
  bson_init(&doc);
  bson_append_document_begin(&doc, "$pull", -1, &child);
  BSON_APPEND_UTF8(&child, "origin", (const char *)origin);
  bson_append_document_end(&doc, &child);

  if (!mongoc_collection_update(collection, MONGOC_UPDATE_MULTI_UPDATE, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error deleting origin information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&doc);
  return ret;
}
  
static int mongo_list_origins(u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("realm"); 

	if(!collection)
    return -1;

  bson_t query, child;
  bson_init(&query);
  bson_append_document_begin(&query, "$orderby", -1, &child);
  BSON_APPEND_INT32(&child, "realm", 1);
  bson_append_document_end(&query, &child);
  bson_append_document_begin(&query, "$query", -1, &child);
  if (realm && realm[0]) {
    BSON_APPEND_UTF8(&child, "realm", (const char *)realm);
  }
  bson_append_document_end(&query, &child);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "origin", 1);
  BSON_APPEND_INT32(&fields, "realm", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'realm'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;

    while (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "realm") && BSON_ITER_HOLDS_UTF8(&iter)) {
        const char * _realm = bson_iter_utf8(&iter, &length);

        if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "origin") && BSON_ITER_HOLDS_ARRAY(&iter)) {
          const uint8_t *docbuf = NULL;
          uint32_t doclen = 0;
          bson_t origin_array;
          bson_iter_t origin_iter;

          bson_iter_array(&iter, &doclen, &docbuf);
          bson_init_static(&origin_array, docbuf, doclen);

          if (bson_iter_init(&origin_iter, &origin_array)) {
            while(bson_iter_next(&origin_iter)) {
              if (BSON_ITER_HOLDS_UTF8(&origin_iter)) {
                const char * _origin = bson_iter_utf8(&origin_iter, &length);
  							printf("%s ==>> %s\n", _realm, _origin);
              }
            }
          }
        }
      }
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static int mongo_set_realm_option_one(u08bits *realm, unsigned long value, const char* opt) {
  mongoc_collection_t * collection = mongo_get_collection("realm"); 

	if(!collection)
    return -1;
    
  bson_t query, doc, child;
  bson_init(&query);
  BSON_APPEND_UTF8(&query, "realm", (const char *)realm);
  bson_init(&doc);
  
  size_t klen = 9 + strlen(opt);
  char * _k = (char *)turn_malloc(klen);
  strcpy(_k, "options.");
  strcat(_k, opt);
  
  if (value > 0) {
    bson_append_document_begin(&doc, "$set", -1, &child);
    BSON_APPEND_INT32(&child, _k, (int32_t)value);
    bson_append_document_end(&doc, &child);
  } else {
    bson_append_document_begin(&doc, "$unset", -1, &child);
    BSON_APPEND_INT32(&child, _k, 1);
    bson_append_document_end(&doc, &child);
  }
  turn_free(_k,klen);
  
  int ret = -1;
  
  if (!mongoc_collection_update(collection, MONGOC_UPDATE_MULTI_UPDATE, &query, &doc, NULL, NULL)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error deleting origin information\n");
  } else {
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&doc);
  return ret;
}
  
static int mongo_list_realm_options(u08bits *realm) {
  mongoc_collection_t * collection = mongo_get_collection("realm"); 

	if(!collection)
    return -1;
    
  bson_t query, child;
  bson_init(&query);
  bson_append_document_begin(&query, "$orderby", -1, &child);
  BSON_APPEND_INT32(&child, "realm", 1);
  bson_append_document_end(&query, &child);
  bson_append_document_begin(&query, "$query", -1, &child);
  if (realm && realm[0]) {
    BSON_APPEND_UTF8(&child, "realm", (const char *)realm);
  }
  bson_append_document_end(&query, &child);

  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "options", 1);
  BSON_APPEND_INT32(&fields, "realm", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);

  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection 'realm'\n");
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;

    while (mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "realm") && BSON_ITER_HOLDS_UTF8(&iter)) {
        const char * _realm = bson_iter_utf8(&iter, &length);

        if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "options") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
          const uint8_t *docbuf = NULL;
          uint32_t doclen = 0;
          bson_t options;
          bson_iter_t options_iter;

          bson_iter_document(&iter, &doclen, &docbuf);
          bson_init_static(&options, docbuf, doclen);

          if (bson_iter_init(&options_iter, &options)) {
            while(bson_iter_next(&options_iter)) {
              const char * _k = bson_iter_key(&options_iter);
              if (BSON_ITER_HOLDS_DOUBLE(&options_iter)) {
                int32_t _v = (int32_t)bson_iter_double(&options_iter);
								printf("%s[%s]=%d\n", _k, _realm, _v);
              } else if (BSON_ITER_HOLDS_INT32(&options_iter)) {
                int32_t _v = bson_iter_int32(&options_iter);
								printf("%s[%s]=%d\n", _k, _realm, _v);
              } else if (BSON_ITER_HOLDS_INT64(&options_iter)) {
                int32_t _v = (int32_t)bson_iter_int64(&options_iter);
								printf("%s[%s]=%d\n", _k, _realm, _v);
              }
            }
          }
        }
      }
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  
static void mongo_auth_ping(void * rch) {
	UNUSED_ARG(rch);
  // NOOP
}
  
static int mongo_get_ip_list(const char *kind, ip_range_list_t * list) {
  char * collection_name = 	(char *)turn_malloc(strlen(kind) + 9);
  sprintf(collection_name, "%s_peer_ip", kind);
  mongoc_collection_t * collection = mongo_get_collection(collection_name); 
  turn_free(collection_name, strlen(kind) + 9);

	if(!collection)
    return -1;
    
  bson_t query;
  bson_init(&query);
  
  bson_t fields;
  bson_init(&fields);
  BSON_APPEND_INT32(&fields, "ip_range", 1);
  
  mongoc_cursor_t * cursor;
  cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &query, &fields, NULL);
  
  int ret = -1;
  
  if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Error querying MongoDB collection '%s'\n", collection_name);
  } else {
    const bson_t * item;
    uint32_t length;
    bson_iter_t iter;
    const char * value;
    while(mongoc_cursor_next(cursor, &item)) {
    	if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "ip_range") && BSON_ITER_HOLDS_UTF8(&iter)) {
        value = bson_iter_utf8(&iter, &length);
		add_ip_list_range(value, list);
      }
    }
    mongoc_cursor_destroy(cursor);
    ret = 0;
  }
  mongoc_collection_destroy(collection);
  bson_destroy(&query);
  bson_destroy(&fields);
  return ret;
}
  

static void mongo_reread_realms(secrets_list_t * realms_list) {

	UNUSED_ARG(realms_list);

	mongoc_collection_t * collection = mongo_get_collection("realm");

	if (!collection)
		return;

	bson_t query;
	bson_init(&query);

	bson_t fields;
	bson_init(&fields);
	BSON_APPEND_INT32(&fields, "realm", 1);
	BSON_APPEND_INT32(&fields, "origin", 1);
	BSON_APPEND_INT32(&fields, "options", 1);

	mongoc_cursor_t * cursor;
	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0,
			&query, &fields, NULL);

	if (!cursor) {
		TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
				"Error querying MongoDB collection 'realm'\n");
	} else {
		ur_string_map *o_to_realm_new = ur_string_map_create(turn_free_simple);

		const bson_t * item;
		uint32_t length;
		bson_iter_t iter;

		while (mongoc_cursor_next(cursor, &item)) {

			if (bson_iter_init(&iter, item) && bson_iter_find(&iter, "realm")
					&& BSON_ITER_HOLDS_UTF8(&iter)) {

				char * _realm = turn_strdup(bson_iter_utf8(&iter, &length));

				get_realm(_realm);

				if (bson_iter_init(&iter, item) && bson_iter_find(&iter,
						"origin") && BSON_ITER_HOLDS_ARRAY(&iter)) {
					const uint8_t *docbuf = NULL;
					uint32_t doclen = 0;
					bson_t origin_array;
					bson_iter_t origin_iter;

					bson_iter_array(&iter, &doclen, &docbuf);
					bson_init_static(&origin_array, docbuf, doclen);

					if (bson_iter_init(&origin_iter, &origin_array)) {
						while (bson_iter_next(&origin_iter)) {
							if (BSON_ITER_HOLDS_UTF8(&origin_iter)) {
								char* _origin =	turn_strdup(bson_iter_utf8(&origin_iter, &length));
								char *rval = turn_strdup(_realm);
								ur_string_map_value_type value =
										(ur_string_map_value_type) (rval);
								ur_string_map_put(o_to_realm_new,
										(const ur_string_map_key_type) _origin,
										value);
								turn_free(_origin,strlen(_origin)+1);
							}
						}
					}
				}

				realm_params_t* rp = get_realm(_realm);
				lock_realms();
				rp->options.perf_options.max_bps = turn_params.max_bps;
				rp->options.perf_options.total_quota = turn_params.total_quota;
				rp->options.perf_options.user_quota = turn_params.user_quota;
				unlock_realms();

				if (bson_iter_init(&iter, item) && bson_iter_find(&iter,
						"options") && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
					const uint8_t *docbuf = NULL;
					uint32_t doclen = 0;
					bson_t options;
					bson_iter_t options_iter;

					bson_iter_document(&iter, &doclen, &docbuf);
					bson_init_static(&options, docbuf, doclen);

					if (bson_iter_init(&options_iter, &options)) {
						while (bson_iter_next(&options_iter)) {
							const char * _k = bson_iter_key(&options_iter);
							int32_t _v = 0;
							if (BSON_ITER_HOLDS_DOUBLE(&options_iter)) {
								_v = (int32_t) bson_iter_double(&options_iter);
							} else if (BSON_ITER_HOLDS_INT32(&options_iter)) {
								_v = bson_iter_int32(&options_iter);
							} else if (BSON_ITER_HOLDS_INT64(&options_iter)) {
								_v = (int32_t) bson_iter_int64(&options_iter);
							}
							if (_v) {
								if (!strcmp(_k, "max-bps"))
									rp->options.perf_options.max_bps
											= (band_limit_t) _v;
								else if (!strcmp(_k, "total-quota"))
									rp->options.perf_options.total_quota
											= (vint) _v;
								else if (!strcmp(_k, "user-quota"))
									rp->options.perf_options.user_quota
											= (vint) _v;
								else {
									TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR,
											"Unknown realm option: %s\n", _k);
								}
							}
						}
					}
				}
				turn_free(_realm,strlen(_realm)+1);
			}
		}
		update_o_to_realm(o_to_realm_new);
		mongoc_cursor_destroy(cursor);
	}
	mongoc_collection_destroy(collection);
	bson_destroy(&query);
	bson_destroy(&fields);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

static turn_dbdriver_t driver = {
  &mongo_get_auth_secrets,
  &mongo_get_user_key,
  &mongo_get_user_pwd,
  &mongo_set_user_key,
  &mongo_set_user_pwd,
  &mongo_del_user,
  &mongo_list_users,
  &mongo_show_secret,
  &mongo_del_secret,
  &mongo_set_secret,
  &mongo_add_origin,
  &mongo_del_origin,
  &mongo_list_origins,
  &mongo_set_realm_option_one,
  &mongo_list_realm_options,
  &mongo_auth_ping,
  &mongo_get_ip_list,
  &mongo_reread_realms,
  &mongo_set_oauth_key,
  &mongo_get_oauth_key,
  &mongo_del_oauth_key,
  &mongo_list_oauth_keys
};

turn_dbdriver_t * get_mongo_dbdriver(void) {
  return &driver;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

#else

turn_dbdriver_t * get_mongo_dbdriver(void) {
  return NULL;
}

#endif