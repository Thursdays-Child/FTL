/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "api.h"
#include "version.h"

#define min(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })

/* qsort comparision function (count field), sort ASC */
int cmpasc(const void *a, const void *b)
{
	int *elem1 = (int*)a;
	int *elem2 = (int*)b;

	if (elem1[1] < elem2[1])
		return -1;
	else if (elem1[1] > elem2[1])
		return 1;
	else
		return 0;
}

// qsort subroutine, sort DESC
int cmpdesc(const void *a, const void *b)
{
	int *elem1 = (int*)a;
	int *elem2 = (int*)b;

	if (elem1[1] > elem2[1])
		return -1;
	else if (elem1[1] < elem2[1])
		return 1;
	else
		return 0;
}

void getStats(int *sock)
{
	int blocked = counters.blocked + counters.wildcardblocked;
	int total = counters.queries - counters.invalidqueries;
	float percentage = 0.0;

	// Avoid 1/0 condition
	if(total > 0)
		percentage = 1e2*blocked/total;

	// Send domains being blocked
	if(istelnet[*sock]) {
		switch(blockingstatus) {
			case 0: // Blocking disabled
				ssend(*sock, "domains_being_blocked N/A\n");
				break;
			default: // Blocking enabled or unknown
				ssend(*sock, "domains_being_blocked %i\n", counters.gravity);
				break;
		}

	}
	else
		pack_int32(*sock, counters.gravity);

	// unique_clients: count only clients that have been active within the most recent 24 hours
	int i, activeclients = 0;
	for(i=0; i < counters.clients; i++)
	{
		validate_access("clients", i, true, __LINE__, __FUNCTION__, __FILE__);
		if(clients[i].count > 0)
			activeclients++;
	}

	if(istelnet[*sock]) {
		ssend(*sock, "dns_queries_today %i\nads_blocked_today %i\nads_percentage_today %f\n",
		      total, blocked, percentage);
		ssend(*sock, "unique_domains %i\nqueries_forwarded %i\nqueries_cached %i\n",
		      counters.domains, counters.forwardedqueries, counters.cached);
		ssend(*sock, "clients_ever_seen %i\n", counters.clients);
		ssend(*sock, "unique_clients %i\n", activeclients);
	}
	else
	{
		pack_int32(*sock, total);
		pack_int32(*sock, blocked);
		pack_float(*sock, percentage);
		pack_int32(*sock, counters.domains);
		pack_int32(*sock, counters.forwardedqueries);
		pack_int32(*sock, counters.cached);
		pack_int32(*sock, counters.clients);
		pack_int32(*sock, activeclients);
	}

	// Send status
	if(istelnet[*sock]) {
		switch(blockingstatus) {
			case 0: // Blocking disabled
				ssend(*sock, "status disabled\n");
				break;
			case 1: // Blocking enabled
				ssend(*sock, "status enabled\n");
				break;
			default: // Unknown status
				ssend(*sock, "status unknown\n");
				break;
		}
	}
	else
		pack_uint8(*sock, blockingstatus);

	if(debugclients)
		logg("Sent stats data to client, ID: %i", *sock);
}

void getOverTime(int *sock)
{
	int i, j = 9999999;

	// Start with the first non-empty overTime slot
	for(i=0; i < counters.overTime; i++)
	{
		validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);
		if(overTime[i].total > 0 || overTime[i].blocked > 0)
		{
			j = i;
			break;
		}
	}

	if(istelnet[*sock])
	{
		for(i = j; i < counters.overTime; i++)
		{
			ssend(*sock,"%i %i %i\n",overTime[i].timestamp,overTime[i].total,overTime[i].blocked);
		}
	}
	else
	{
		// We can use the map16 type because there should only be about 288 time slots (TIMEFRAME set to "yesterday")
		// and map16 can hold up to (2^16)-1 = 65535 pairs

		// Send domains over time
		pack_map16_start(*sock, (uint16_t) (counters.overTime - j));
		for(i = j; i < counters.overTime; i++) {
			pack_int32(*sock, overTime[i].timestamp);
			pack_int32(*sock, overTime[i].total);
		}

		// Send ads over time
		pack_map16_start(*sock, (uint16_t) (counters.overTime - j));
		for(i = j; i < counters.overTime; i++) {
			pack_int32(*sock, overTime[i].timestamp);
			pack_int32(*sock, overTime[i].blocked);
		}
	}

	if(debugclients)
		logg("Sent overTime data to client, ID: %i", *sock);
}

void getTopDomains(char *client_message, int *sock)
{
	int i, temparray[counters.domains][2], count=10, num;
	bool blocked, audit = false, asc = false;

	blocked = command(client_message, ">top-ads");

	// Exit before processing any data if requested via config setting
	if(!config.query_display)
		return;

	// Match both top-domains and top-ads
	// example: >top-domains (15)
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		count = num;
	}

	// Apply Audit Log filtering?
	// example: >top-domains for audit
	if(command(client_message, " for audit"))
		audit = true;

	// Sort in ascending order?
	// example: >top-domains asc
	if(command(client_message, " asc"))
		asc = true;

	for(i=0; i < counters.domains; i++)
	{
		validate_access("domains", i, true, __LINE__, __FUNCTION__, __FILE__);
		temparray[i][0] = i;
		if(blocked)
			temparray[i][1] = domains[i].blockedcount;
		else
			// Count only permitted queries
			temparray[i][1] = (domains[i].count - domains[i].blockedcount);
	}

	// Sort temporary array
	if(asc)
		qsort(temparray, counters.domains, sizeof(int[2]), cmpasc);
	else
		qsort(temparray, counters.domains, sizeof(int[2]), cmpdesc);


	// Get filter
	char * filter = read_setupVarsconf("API_QUERY_LOG_SHOW");
	bool showpermitted = true, showblocked = true;
	if(filter != NULL)
	{
		if((strcmp(filter, "permittedonly")) == 0)
			showblocked = false;
		else if((strcmp(filter, "blockedonly")) == 0)
			showpermitted = false;
		else if((strcmp(filter, "nothing")) == 0)
		{
			showpermitted = false;
			showblocked = false;
		}
	}
	clearSetupVarsArray();

	// Get domains which the user doesn't want to see
	char * excludedomains = NULL;
	if(!audit)
	{
		excludedomains = read_setupVarsconf("API_EXCLUDE_DOMAINS");
		if(excludedomains != NULL)
		{
			getSetupVarsArray(excludedomains);

			if(debugclients)
				logg("Excluding %i domains from being displayed", setupVarsElements);
		}
	}

	if(!istelnet[*sock])
	{
		// Send the data required to get the percentage each domain has been blocked / queried
		if(blocked)
			pack_int32(*sock, counters.blocked);
		else
			pack_int32(*sock, counters.queries - counters.invalidqueries);
	}

	int n = 0;
	for(i=0; i < counters.domains; i++)
	{
		// Get sorted indices
		int j = temparray[i][0];
		validate_access("domains", j, true, __LINE__, __FUNCTION__, __FILE__);

		// Skip this domain if there is a filter on it
		if(excludedomains != NULL && insetupVarsArray(domains[j].domain))
			continue;

		// Skip this domain if already included in audit
		if(audit && countlineswith(domains[j].domain, files.auditlist) > 0)
			continue;

		if(blocked && showblocked && domains[j].blockedcount > 0)
		{
			if(audit && domains[j].wildcard)
			{
				if(istelnet[*sock])
					ssend(*sock, "%i %i %s wildcard\n", n, domains[j].blockedcount, domains[j].domain);
				else {
					char *fancyWildcard = calloc(3 + strlen(domains[j].domain), sizeof(char));
					sprintf(fancyWildcard, "*.%s", domains[j].domain);

					if(!pack_str32(*sock, fancyWildcard))
						return;

					pack_int32(*sock, domains[j].blockedcount);
					free(fancyWildcard);
				}
			}
			else
			{
				if(istelnet[*sock])
					ssend(*sock, "%i %i %s\n", n, domains[j].blockedcount, domains[j].domain);
				else {
					if(!pack_str32(*sock, domains[j].domain))
						return;

					pack_int32(*sock, domains[j].blockedcount);
				}
			}
			n++;
		}
		else if(!blocked && showpermitted && (domains[j].count - domains[j].blockedcount) > 0)
		{
			if(istelnet[*sock])
				ssend(*sock,"%i %i %s\n",n,(domains[j].count - domains[j].blockedcount),domains[j].domain);
			else
			{
				if(!pack_str32(*sock, domains[j].domain))
					return;

				pack_int32(*sock, domains[j].count - domains[j].blockedcount);
			}
			n++;
		}

		// Only count entries that are actually sent and return when we have send enough data
		if(n == count)
			break;
	}

	if(excludedomains != NULL)
		clearSetupVarsArray();

	if(debugclients)
	{
		if(blocked)
			logg("Sent top ads list data to client, ID: %i", *sock);
		else
			logg("Sent top domains list data to client, ID: %i", *sock);
	}
}

void getTopClients(char *client_message, int *sock)
{
	int i, temparray[counters.clients][2], count=10, num;

	// Match both top-domains and top-ads
	// example: >top-clients (15)
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		count = num;
	}

	// Show also clients which have not been active recently?
	// This option can be combined with existing options,
	// i.e. both >top-clients withzero" and ">top-clients withzero (123)" are valid
	bool includezeroclients = false;
	if(command(client_message, " withzero"))
		includezeroclients = true;

	for(i=0; i < counters.clients; i++)
	{
		validate_access("clients", i, true, __LINE__, __FUNCTION__, __FILE__);
		temparray[i][0] = i;
		temparray[i][1] = clients[i].count;
	}

	// Sort in ascending order?
	// example: >top-clients asc
	bool asc = false;
	if(command(client_message, " asc"))
		asc = true;

	// Sort temporary array
	if(asc)
		qsort(temparray, counters.clients, sizeof(int[2]), cmpasc);
	else
		qsort(temparray, counters.clients, sizeof(int[2]), cmpdesc);

	// Get clients which the user doesn't want to see
	char * excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);

		if(debugclients)
			logg("Excluding %i clients from being displayed", setupVarsElements);
	}

	if(!istelnet[*sock])
	{
		// Send the total queries so they can make percentages from this data
		pack_int32(*sock, counters.queries - counters.invalidqueries);
	}

	int n = 0;
	for(i=0; i < counters.clients; i++)
	{
		// Get sorted indices
		int j = temparray[i][0];
		validate_access("clients", j, true, __LINE__, __FUNCTION__, __FILE__);

		// Skip this client if there is a filter on it
		if(excludeclients != NULL && (insetupVarsArray(clients[j].ip) || insetupVarsArray(clients[j].name)))
			continue;

		// Return this client if either
		// - "withzero" option is set, and/or
		// - the client made at least one query within the most recent 24 hours
		if(includezeroclients || clients[j].count > 0)
		{
			if(istelnet[*sock])
				ssend(*sock,"%i %i %s %s\n",n,clients[j].count,clients[j].ip,clients[j].name);
			else
			{
				if(!pack_str32(*sock, clients[j].name) || !pack_str32(*sock, clients[j].ip))
					return;

				pack_int32(*sock, clients[j].count);
			}
			n++;
		}

		if(n == count)
			break;
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();

	if(debugclients)
		logg("Sent top clients data to client, ID: %i", *sock);
}


void getForwardDestinations(char *client_message, int *sock)
{
	bool allocated = false, sort = true;
	int i, temparray[counters.forwarded+1][2], forwardedsum = 0, totalqueries = 0;

	if(command(client_message, "unsorted"))
		sort = false;

	for(i=0; i < counters.forwarded; i++) {
		validate_access("forwarded", i, true, __LINE__, __FUNCTION__, __FILE__);
		// Compute forwardedsum
		forwardedsum += forwarded[i].count;

		// If we want to print a sorted output, we fill the temporary array with
		// the values we will use for sorting afterwards
		if(sort) {
			temparray[i][0] = i;
			temparray[i][1] = forwarded[i].count;
		}
	}

	if(sort) {
		// Add "local " forward destination
		temparray[counters.forwarded][0] = counters.forwarded;
		temparray[counters.forwarded][1] = counters.cached + counters.blocked;

		// Sort temporary array in descending order
		qsort(temparray, counters.forwarded+1, sizeof(int[2]), cmpdesc);
	}

	totalqueries = counters.forwardedqueries + counters.cached + counters.blocked;

	// Loop over available forward destinations
	for(i=0; i < min(counters.forwarded+1, 10); i++)
	{
		char *name, *ip;
		double percentage;

		// Get sorted indices
		int j;
		if(sort)
			j = temparray[i][0];
		else
			j = i;

		// Is this the "local" forward destination?
		if(j == counters.forwarded)
		{
			ip = calloc(4,1);
			strcpy(ip, "::1");
			name = calloc(6,1);
			strcpy(name, "local");

			if(totalqueries > 0)
				// Whats the percentage of (cached + blocked) queries on the total amount of queries?
				percentage = 1e2 * (counters.cached + counters.blocked) / totalqueries;
			else
				percentage = 0.0;

			allocated = true;
		}
		else
		{
			validate_access("forwarded", j, true, __LINE__, __FUNCTION__, __FILE__);
			ip = forwarded[j].ip;
			name = forwarded[j].name;

			// Math explanation:
			// A single query may result in requests being forwarded to multiple destinations
			// Hence, in order to be able to give percentages here, we have to normalize the
			// number of forwards to each specific destination by the total number of forward
			// events. This term is done by
			//   a = forwarded[j].count / forwardedsum
			//
			// The fraction a describes now how much share an individual forward destination
			// has on the total sum of sent requests.
			// We also know the share of forwarded queries on the total number of queries
			//   b = counters.forwardedqueries / c
			// where c is the number of valid queries,
			//   c = counters.forwardedqueries + counters.cached + counters.blocked
			//
			// To get the total percentage of a specific query on the total number of queries,
			// we simply have to scale b by a which is what we do in the following.
			if(forwardedsum > 0 && totalqueries > 0)
				percentage = 1e2 * forwarded[j].count / forwardedsum * counters.forwardedqueries / totalqueries;
			else
				percentage = 0.0;

			allocated = false;
		}

		// Send data if count > 0
		if(percentage > 0.0)
		{
			if(istelnet[*sock])
				ssend(*sock, "%i %.2f %s %s\n", i, percentage, ip, name);
			else
			{
				if(!pack_str32(*sock, name) || !pack_str32(*sock, ip))
					return;

				pack_float(*sock, (float) percentage);
			}
		}

		// Free previously allocated memory only if we allocated it
		if(allocated)
		{
			free(ip);
			free(name);
		}
	}

	if(debugclients)
		logg("Sent forward destination data to client, ID: %i", *sock);
}


void getQueryTypes(int *sock)
{
	int total = counters.IPv4 + counters.IPv6;
	double percentageIPv4 = 0.0, percentageIPv6 = 0.0;

	// Prevent floating point exceptions by checking if the divisor is != 0
	if(total > 0) {
		percentageIPv4 = 1e2*counters.IPv4/total;
		percentageIPv6 = 1e2*counters.IPv6/total;
	}

	if(istelnet[*sock])
		ssend(*sock,"A (IPv4): %.2f\nAAAA (IPv6): %.2f\n", percentageIPv4, percentageIPv6);
	else {
		pack_float(*sock, (float) percentageIPv4);
		pack_float(*sock, (float) percentageIPv6);
	}

	if(debugclients)
		logg("Sent query type data to client, ID: %i", *sock);
}


void getAllQueries(char *client_message, int *sock)
{
	// Exit before processing any data if requested via config setting
	if(!config.query_display)
		return;

	// Do we want a more specific version of this command (domain/client/time interval filtered)?
	int from = 0, until = 0;

	char *domainname = NULL;
	bool filterdomainname = false;

	char *clientname = NULL;
	bool filterclientname = false;

	// Time filtering?
	if(command(client_message, ">getallqueries-time")) {
		sscanf(client_message, ">getallqueries-time %i %i",&from, &until);
	}
	// Domain filtering?
	if(command(client_message, ">getallqueries-domain")) {
		// Get domain name we want to see only (limit length to 255 chars)
		domainname = calloc(256, sizeof(char));
		sscanf(client_message, ">getallqueries-domain %255s", domainname);
		if(debugclients)
			logg("Showing only queries with domain %s", domainname);
		filterdomainname = true;
	}
	// Client filtering?
	if(command(client_message, ">getallqueries-client")) {
		clientname = calloc(256, sizeof(char));
		// Get client name we want to see only (limit length to 255 chars)
		sscanf(client_message, ">getallqueries-client %255s", clientname);
		if(debugclients)
			logg("Showing only queries with client %s", clientname);
		filterclientname = true;
	}

	int ibeg = 0, num;
	// Test for integer that specifies number of entries to be shown
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0)
	{
		// User wants a different number of requests
		// Don't allow a start index that is smaller than zero
		ibeg = counters.queries-num;
		if(ibeg < 0)
			ibeg = 0;
	}

	// Get potentially existing filtering flags
	char * filter = read_setupVarsconf("API_QUERY_LOG_SHOW");
	bool showpermitted = true, showblocked = true;
	if(filter != NULL)
	{
		if((strcmp(filter, "permittedonly")) == 0)
			showblocked = false;
		else if((strcmp(filter, "blockedonly")) == 0)
			showpermitted = false;
		else if((strcmp(filter, "nothing")) == 0)
		{
			showpermitted = false;
			showblocked = false;
		}
	}
	clearSetupVarsArray();

	// Get privacy mode flag
	char * privacy = read_setupVarsconf("API_PRIVACY_MODE");
	bool privacymode = false;

	if(privacy != NULL)
		if(getSetupVarsBool(privacy))
			privacymode = true;

	clearSetupVarsArray();

	if(debugclients)
	{
		if(showpermitted)
			logg("Showing permitted queries");
		else
			logg("Hiding permitted queries");

		if(showblocked)
			logg("Showing blocked queries");
		else
			logg("Hiding blocked queries");

		if(privacymode)
			logg("Privacy mode enabled");
	}

	int i;
	for(i=ibeg; i < counters.queries; i++)
	{
		validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);
		// Check if this query has been removed due to garbage collection
		if(!queries[i].valid) continue;

		validate_access("domains", queries[i].domainID, true, __LINE__, __FUNCTION__, __FILE__);
		validate_access("clients", queries[i].clientID, true, __LINE__, __FUNCTION__, __FILE__);

		char qtype[5];
		if(queries[i].type == 1)
			strcpy(qtype,"IPv4");
		else
			strcpy(qtype,"IPv6");

		if((queries[i].status == 1 || queries[i].status == 4) && !showblocked)
			continue;
		if((queries[i].status == 2 || queries[i].status == 3) && !showpermitted)
			continue;

		// Skip those entries which so not meet the requested timeframe
		if((from > queries[i].timestamp && from != 0) || (queries[i].timestamp > until && until != 0))
			continue;

		if(filterdomainname)
		{
			// Skip if domain name is not identical with what the user wants to see
			if(strcmp(domains[queries[i].domainID].domain, domainname) != 0)
				continue;
		}

		if(filterclientname)
		{
			// Skip if client name and IP are not identical with what the user wants to see
			if((strcmp(clients[queries[i].clientID].ip, clientname) != 0) &&
			   (strcmp(clients[queries[i].clientID].name, clientname) != 0))
				continue;
		}

		if(istelnet[*sock])
		{
			if(!privacymode)
			{
				if(strlen(clients[queries[i].clientID].name) > 0)
					ssend(*sock,"%i %s %s %s %i %i\n",queries[i].timestamp,qtype,domains[queries[i].domainID].domain,clients[queries[i].clientID].name,queries[i].status,domains[queries[i].domainID].dnssec);
				else
					ssend(*sock,"%i %s %s %s %i %i\n",queries[i].timestamp,qtype,domains[queries[i].domainID].domain,clients[queries[i].clientID].ip,queries[i].status,domains[queries[i].domainID].dnssec);
			}
			else
				ssend(*sock,"%i %s %s hidden %i %i\n",queries[i].timestamp,qtype,domains[queries[i].domainID].domain,queries[i].status,domains[queries[i].domainID].dnssec);
		}
		else
		{
			char *client;

			if(!privacymode) {
				if(strlen(clients[queries[i].clientID].name) > 0)
					client = clients[queries[i].clientID].name;
				else
					client = clients[queries[i].clientID].ip;
			}
			else
				client = "hidden";

			pack_int32(*sock, queries[i].timestamp);

			// Use a fixstr because the length of qtype is always 4 (max is 31 for fixstr)
			if(!pack_fixstr(*sock, qtype))
				return;

			// Use str32 for domain and client because we have no idea how long they will be (max is 4294967295 for str32)
			if(!pack_str32(*sock, domains[queries[i].domainID].domain) || !pack_str32(*sock, client))
				return;

			pack_uint8(*sock, queries[i].status);
			pack_uint8(*sock, domains[queries[i].domainID].dnssec);
		}
	}

	// Free allocated memory
	if(filterclientname)
		free(clientname);

	if(filterdomainname)
		free(domainname);

	if(debugclients)
		logg("Sent all queries data to client, ID: %i", *sock);
}

void getRecentBlocked(char *client_message, int *sock)
{
	int i, num=1;

	// Exit before processing any data if requested via config setting
	if(!config.query_display)
		return;

	// Test for integer that specifies number of entries to be shown
	if(sscanf(client_message, "%*[^(](%i)", &num) > 0) {
		// User wants a different number of requests
		if(num >= counters.queries)
			num = 0;
	}

	// Find most recent query with either status 1 (blocked)
	// or status 4 (wildcard blocked)
	int found = 0;
	for(i = counters.queries - 1; i > 0 ; i--)
	{
		validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);
		// Check if this query has been removed due to garbage collection
		if(!queries[i].valid) continue;

		if(queries[i].status == 1 || queries[i].status == 4)
		{
			found++;

			if(istelnet[*sock])
				ssend(*sock,"%s\n", domains[queries[i].domainID].domain);
			else if(!pack_str32(*sock, domains[queries[i].domainID].domain))
				return;
		}

		if(found >= num)
			break;
	}
}

void getMemoryUsage(int *sock)
{
	unsigned long int structbytes = sizeof(countersStruct) + sizeof(ConfigStruct) + counters.queries_MAX*sizeof(queriesDataStruct) + counters.forwarded_MAX*sizeof(forwardedDataStruct) + counters.clients_MAX*sizeof(clientsDataStruct) + counters.domains_MAX*sizeof(domainsDataStruct) + counters.overTime_MAX*sizeof(overTimeDataStruct) + (counters.wildcarddomains)*sizeof(*wildcarddomains);
	char *structprefix = calloc(2, sizeof(char));
	double formated = 0.0;
	format_memory_size(structprefix, structbytes, &formated);

	if(istelnet[*sock])
		ssend(*sock,"memory allocated for internal data structure: %lu bytes (%.2f %sB)\n",structbytes,formated,structprefix);
	else
		pack_uint64(*sock, structbytes);
	free(structprefix);

	unsigned long int dynamicbytes = memory.wildcarddomains + memory.domainnames + memory.clientips + memory.clientnames + memory.forwardedips + memory.forwardednames + memory.forwarddata;
	char *dynamicprefix = calloc(2, sizeof(char));
	format_memory_size(dynamicprefix, dynamicbytes, &formated);

	if(istelnet[*sock])
		ssend(*sock,"dynamically allocated allocated memory used for strings: %lu bytes (%.2f %sB)\n",dynamicbytes,formated,dynamicprefix);
	else
		pack_uint64(*sock, dynamicbytes);
	free(dynamicprefix);

	unsigned long int totalbytes = structbytes + dynamicbytes;
	char *totalprefix = calloc(2, sizeof(char));
	format_memory_size(totalprefix, totalbytes, &formated);

	if(istelnet[*sock])
		ssend(*sock,"Sum: %lu bytes (%.2f %sB)\n",totalbytes,formated,totalprefix);
	else
		pack_uint64(*sock, totalbytes);
	free(totalprefix);

	if(debugclients)
		logg("Sent memory data to client, ID: %i", *sock);
}

void getForwardDestinationsOverTime(int *sock)
{
	int i, sendit = -1;

	for(i = 0; i < counters.overTime; i++)
	{
		validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);
		if((overTime[i].total > 0 || overTime[i].blocked > 0))
		{
			sendit = i;
			break;
		}
	}

	// Send the number of forward destinations (number of items for each timestamp), names, and IPs
	if(!istelnet[*sock]) {
		// Add one to include the local forwarded category
		pack_int32(*sock, counters.forwarded + 1);

		for(i = 0; i < counters.forwarded + 1; i++) {
			char *name, *ip;

			if(i == counters.forwarded) {
				name = "local";
				ip = "::1";
			}
			else {
				validate_access("forwarded", i, true, __LINE__, __FUNCTION__, __FILE__);
				name = forwarded[i].name;
				ip = forwarded[i].ip;
			}

			if(!pack_str32(*sock, name) || !pack_str32(*sock, ip))
				return;
		}
	}

	if(sendit > -1)
	{
		for(i = sendit; i < counters.overTime; i++)
		{
			float percentage;

			validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);
			if(istelnet[*sock])
			{
				ssend(*sock, "%i", overTime[i].timestamp);
			}
			else
			{
				pack_int32(*sock, overTime[i].timestamp);
			}

			int j, forwardedsum = 0;

			// Compute forwardedsum used for later normalization
			for(j = 0; j < overTime[i].forwardnum; j++)
			{
				forwardedsum += overTime[i].forwarddata[j];
			}

			// Loop over forward destinations to generate output to be sent to the client
			for(j = 0; j < counters.forwarded; j++)
			{
				int thisforward = 0;

				if(j < overTime[i].forwardnum) {
					// This forward destination does already exist at this timestamp
					// -> use counter of requests sent to this destination
					thisforward = overTime[i].forwarddata[j];
				}
				// else
				// {
					// This forward destination does not yet exist at this timestamp
					// -> use zero as number of requests sent to this destination
				// 	thisforward = 0;
				// }

				// Avoid floating point exceptions
				if(forwardedsum > 0 && overTime[i].total > 0 && thisforward > 0) {
					// A single query may result in requests being forwarded to multiple destinations
					// Hence, in order to be able to give percentages here, we have to normalize the
					// number of forwards to each specific destination by the total number of forward
					// events. This is done by
					//   a = thisforward / forwardedsum
					// The fraction a describes how much share an individual forward destination
					// has on the total sum of sent requests.
					//
					// We also know the share of forwarded queries on the total number of queries
					//   b = forwardedqueries/overTime[i].total
					// where the number of forwarded queries in this time interval is given by
					//   forwardedqueries = overTime[i].total - (overTime[i].cached
					//                                           + overTime[i].blocked)
					//
					// To get the total percentage of a specific forward destination on the total
					// number of queries, we simply have to multiply a and b as done below:
					percentage = (float) (1e2 * thisforward / forwardedsum * (overTime[i].total - (overTime[i].cached + overTime[i].blocked)) / overTime[i].total);
				}
				else
					percentage = 0.0;

				if(istelnet[*sock])
					ssend(*sock, " %.2f", percentage);
				else
					pack_float(*sock, (float) percentage);
			}

			// Avoid floating point exceptions
			if(overTime[i].total > 0)
				// Forward count for destination "local" is cached + blocked normalized by total:
				percentage = (float) (1e2 * (overTime[i].cached + overTime[i].blocked) / overTime[i].total);
			else
				percentage = 0.0;

			if(istelnet[*sock])
				ssend(*sock, " %.2f\n", percentage);
			else
				pack_float(*sock, percentage);
		}
	}

	if(debugclients)
		logg("Sent overTime forwarded data to client, ID: %i", *sock);
}

void getClientID(int *sock)
{
	if(istelnet[*sock])
		ssend(*sock,"%i\n", *sock);
	else
		pack_int32(*sock, *sock);

	if(debugclients)
		logg("Sent client ID to client, ID: %i", *sock);
}

void getQueryTypesOverTime(int *sock)
{
	int i, sendit = -1;
	for(i = 0; i < counters.overTime; i++)
	{
		validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);
		if((overTime[i].total > 0 || overTime[i].blocked > 0))
		{
			sendit = i;
			break;
		}
	}

	if(sendit > -1)
	{
		for(i = sendit; i < counters.overTime; i++)
		{
			validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);

			float percentageIPv4 = 0.0, percentageIPv6 = 0.0;
			int sum = overTime[i].querytypedata[0] + overTime[i].querytypedata[1];

			if(sum > 0) {
				percentageIPv4 = (float) (1e2 * overTime[i].querytypedata[0] / sum);
				percentageIPv6 = (float) (1e2 * overTime[i].querytypedata[1] / sum);
			}

			if(istelnet[*sock])
				ssend(*sock, "%i %.2f %.2f\n", overTime[i].timestamp, percentageIPv4, percentageIPv6);
			else {
				pack_int32(*sock, overTime[i].timestamp);
				pack_float(*sock, percentageIPv4);
				pack_float(*sock, percentageIPv6);
			}
		}
	}

	if(debugclients)
		logg("Sent overTime query types data to client, ID: %i", *sock);
}

void getVersion(int *sock)
{
	const char * commit = GIT_HASH;
	const char * tag = GIT_TAG;

	if(strlen(tag) > 1) {
		if(istelnet[*sock])
			ssend(*sock, "version %s\ntag %s\nbranch %s\ndate %s\n", GIT_VERSION, tag, GIT_BRANCH, GIT_DATE);
		else {
			if(!pack_str32(*sock, GIT_VERSION) ||
					!pack_str32(*sock, (char *) tag) ||
					!pack_str32(*sock, GIT_BRANCH) ||
					!pack_str32(*sock, GIT_DATE))
				return;
		}
	}
	else {
		char hash[8];
		// Extract first 7 characters of the hash
		strncpy(hash, commit, 7); hash[7] = 0;

		if(istelnet[*sock])
			ssend(*sock, "version vDev-%s\ntag %s\nbranch %s\ndate %s\n", hash, tag, GIT_BRANCH, GIT_DATE);
		else {
			char *hashVersion = calloc(6 + strlen(hash), sizeof(char));
			sprintf(hashVersion, "vDev-%s", hash);

			if(!pack_str32(*sock, hashVersion) ||
					!pack_str32(*sock, (char *) tag) ||
					!pack_str32(*sock, GIT_BRANCH) ||
					!pack_str32(*sock, GIT_DATE))
				return;

			free(hashVersion);
		}
	}

	if(debugclients)
		logg("Sent version info to client, ID: %i", *sock);
}

void getDBstats(int *sock)
{
	// Get file details
	struct stat st;
	long int filesize = 0;
	if(stat(FTLfiles.db, &st) != 0)
		// stat() failed (maybe the file does not exist?)
		filesize = -1;
	else
		filesize = st.st_size;

	char *prefix = calloc(2, sizeof(char));
	double formated = 0.0;
	format_memory_size(prefix, filesize, &formated);

	if(istelnet[*sock])
		ssend(*sock,"queries in database: %i\ndatabase filesize: %.2f %sB\nSQLite version: %s\n", get_number_of_queries_in_DB(), formated, prefix, sqlite3_libversion());
	else {
		pack_int32(*sock, get_number_of_queries_in_DB());
		pack_int64(*sock, filesize);

		if(!pack_str32(*sock, (char *) sqlite3_libversion()))
			return;
	}

	if(debugclients)
		logg("Sent DB info to client, ID: %i", *sock);
}

void getClientsOverTime(int *sock)
{
	int i, sendit = -1;

	for(i = 0; i < counters.overTime; i++)
	{
		validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);
		if((overTime[i].total > 0 || overTime[i].blocked > 0))
		{
			sendit = i;
			break;
		}
	}
	if(sendit < 0)
		return;

	// Get clients which the user doesn't want to see
	char * excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	// Array of clients to be skipped in the output
	// if skipclient[i] == true then this client should be hidden from
	// returned data. We initialize it with false
	bool skipclient[counters.clients];
	memset(skipclient, false, counters.clients*sizeof(bool));

	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);

		for(i=0; i < counters.clients; i++)
		{
			validate_access("clients", i, true, __LINE__, __FUNCTION__, __FILE__);
			// Check if this client should be skipped
			if(insetupVarsArray(clients[i].ip) || insetupVarsArray(clients[i].name))
			{
				skipclient[i] = true;
			}
		}
	}

	// Main return loop
	for(i = sendit; i < counters.overTime; i++)
	{
		validate_access("overTime", i, true, __LINE__, __FUNCTION__, __FILE__);

		if(istelnet[*sock])
			ssend(*sock, "%i", overTime[i].timestamp);
		else
			pack_int32(*sock, overTime[i].timestamp);

		// Loop over forward destinations to generate output to be sent to the client
		int j;
		for(j = 0; j < counters.clients; j++)
		{
			int thisclient = 0;

			if(skipclient[j])
				continue;

			if(j < overTime[i].clientnum)
			{
				// This client entry does already exist at this timestamp
				// -> use counter of requests sent to this destination
				thisclient = overTime[i].clientdata[j];
			}

			if(istelnet[*sock])
				ssend(*sock, " %i", thisclient);
			else
				pack_int32(*sock, thisclient);
		}

		if(istelnet[*sock])
			ssend(*sock, "\n");
		else
			pack_int32(*sock, -1);
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();
}

void getClientNames(int *sock)
{
	int i;

	// Get clients which the user doesn't want to see
	char * excludeclients = read_setupVarsconf("API_EXCLUDE_CLIENTS");
	// Array of clients to be skipped in the output
	// if skipclient[i] == true then this client should be hidden from
	// returned data. We initialize it with false
	bool skipclient[counters.clients];
	memset(skipclient, false, counters.clients*sizeof(bool));

	if(excludeclients != NULL)
	{
		getSetupVarsArray(excludeclients);

		for(i=0; i < counters.clients; i++)
		{
			validate_access("clients", i, true, __LINE__, __FUNCTION__, __FILE__);
			// Check if this client should be skipped

		}
	}

	// Loop over clients to generate output to be sent to the client
	for(i = 0; i < counters.clients; i++)
	{
		validate_access("clients", i, true, __LINE__, __FUNCTION__, __FILE__);
		if(insetupVarsArray(clients[i].ip) || insetupVarsArray(clients[i].name))
			continue;

		if(istelnet[*sock])
			ssend(*sock, "%i %i %s %s\n", i, clients[i].count, clients[i].ip, clients[i].name);
		else {
			if(!pack_str32(*sock, clients[i].name) || !pack_str32(*sock, clients[i].ip))
				return;

			pack_int32(*sock, clients[i].count);
		}
	}

	if(excludeclients != NULL)
		clearSetupVarsArray();
}

void getUnknownQueries(int *sock)
{
	int i;
	for(i=0; i < counters.queries; i++)
	{
		validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);
		// Check if this query has been removed due to garbage collection
		if(queries[i].status != 0 && queries[i].complete) continue;

		char type[5];
		if(queries[i].type == 1)
		{
			strcpy(type,"IPv4");
		}
		else
		{
			strcpy(type,"IPv6");
		}

		validate_access("domains", queries[i].domainID, true, __LINE__, __FUNCTION__, __FILE__);
		validate_access("clients", queries[i].clientID, true, __LINE__, __FUNCTION__, __FILE__);


		char *client;

		if(strlen(clients[queries[i].clientID].name) > 0)
			client = clients[queries[i].clientID].name;
		else
			client = clients[queries[i].clientID].ip;

		if(istelnet[*sock])
			ssend(*sock, "%i %i %i %s %s %s %i %s\n", queries[i].timestamp, i, queries[i].id, type, domains[queries[i].domainID].domain, client, queries[i].status, queries[i].complete ? "true" : "false");
		else {
			pack_int32(*sock, queries[i].timestamp);
			pack_int32(*sock, queries[i].id);

			// Use a fixstr because the length of qtype is always 4 (max is 31 for fixstr)
			if(!pack_fixstr(*sock, type))
				return;

			// Use str32 for domain and client because we have no idea how long they will be (max is 4294967295 for str32)
			if(!pack_str32(*sock, domains[queries[i].domainID].domain) || !pack_str32(*sock, client))
				return;

			pack_uint8(*sock, queries[i].status);
			pack_bool(*sock, queries[i].complete);
		}
	}

	if(debugclients)
		logg("Sent unknown queries data to client, ID: %i", *sock);
}
