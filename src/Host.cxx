/*
   Copyright 2017 Steven Hessing

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

 * Host.cpp
 *
 *  Created on: Mar 6, 2017
 *      Author: steven
 */

#include <cstring>
#include <ctime>
#include <string>
#include <forward_list>

#include <syslog.h>

// #include <curl/curl.h>
// #include "log/Log.h"
#include <json.hpp>
using json = nlohmann::json;
#include "Host.h"
#include "DhcpRequest.h"
#include "SsdpLocation.h"
#include "FlowEntry.h"
#include "SsdpHost.h"
#include "DeviceProfile.h"

bool Host::Match(const DeviceProfileMap& dpMap) {
	// If already matched and time of match is more recent as last update in Device Profiles then keep
	// keep current match, accepting that there may be a better match available. I'm assuming that
	// matching will be expensive process on HGWs so if we really want to force certain devices to
	// re-match then update the LastUpdated value of the correspondign DeviceProfile
	if (isMatched()) {
		auto it = dpMap.find(Uuid);
		if (it != dpMap.end()) {
			if (matchtime > it->second->LastUpdated_get()) {
				return true;
			}
		}
	}
	ConfidenceLevel bestmatch = ConfidenceLevel::None;
	std::string matcheduuid = "";
	for (auto &kv : dpMap) {
		syslog(LOG_DEBUG, "Evaluating host %s against device profile %s", MacAddress.c_str(), kv.first.c_str());
		auto &dp = *(kv.second);
		auto match = Match(dp);
		if (match > bestmatch) {
			bestmatch = match;
			matcheduuid = kv.first;
			matchtime = time(nullptr);
		}

	}
	if (bestmatch >= ConfidenceLevel::Low) {
		Uuid = matcheduuid;
		return true;
	}
	return false;
}

ConfidenceLevel Host::Match(const DeviceProfile& dp) {
	ConfidenceLevel bestmatch = ConfidenceLevel::None;
	auto v = dp.Identifiers_get();
	for (auto& i : v) {
		auto match = Match(*i);
		if (match >= ConfidenceLevel::High)
			return match;

		if 	(match > bestmatch)
			bestmatch = match;
	}
	return bestmatch;
}

ConfidenceLevel Host::Match(const Identifier& i) {
	for (auto& mc : i.MatchConditions_get()) {
		if(! Match (*mc)) {
			return ConfidenceLevel::None;
		}
	}
	return i.IdentifyConfidenceLevel_get();
}

bool Host::Match(const MatchCondition& mc) {
	if (mc.SubsetMatch)
		return MatchSubset(mc);

	std::string value;
	if (mc.Key == "MacOid")
		value = MacAddress.substr(0,8);
	if (mc.Key == "DhcpHostname")
		value = Dhcp.DhcpHostname;
	else if (mc.Key == "DhcpVendor")
		value = Dhcp.DhcpVendor;
	else if (mc.Key == "DhcpHostname")
		value = Dhcp.DhcpHostname ;
	else if (mc.Key == "Hostname")
		value = Dhcp.Hostname ;
	else if (mc.Key == "SsdpFriendlyName" )
		value = Ssdp.FriendlyName;
	else if (mc.Key == "SsdpManufacturer")
		value = Ssdp.Manufacturer;
	else if (mc.Key == "SsdpManufacturerUrl")
		value = Ssdp.ManufacturerUrl;
	else if (mc.Key == "SsdpModelName")
		value = Ssdp.ModelName;
	else if (mc.Key == "SsdpModelUrl")
		value = Ssdp.ModelUrl;
	else if (mc.Key == "SsdpSerialNumber")
		value = Ssdp.SerialNumber;
	else if (mc.Key == "SsdpUserAgent")
		value = Ssdp.UserAgent;
	else if (mc.Key == "SsdpServer")
		value = Ssdp.Server;
	else if (mc.Key == "SsdpLocation")
		value = Ssdp.Location;

	size_t startpos = 0;
	size_t mcvaluelength = mc.Value.length();
	size_t datavaluelength = value.length();
	std::string mcvalue = mc.Value;
	if (mc.Key[0] == '*') {
		mcvalue = mcvalue.substr(1);
		startpos = datavaluelength - mcvalue.length();
	} else if (mc.Value[mcvaluelength-1] == '*') {
		mcvalue = mcvalue.substr(0,mcvaluelength-1);
		startpos = 0;
	}
	syslog (LOG_DEBUG, "Evaluating %s to match condition %s with value %s from position %lu", value.c_str(), mc.Key.c_str(), mcvalue.c_str(), startpos);
	if (value.compare(startpos, mcvalue.length() - startpos, mcvalue) == 0)
		return true;

	return false;
}

bool Host::MatchSubset(const MatchCondition& mc) {

	if(mc.Key == "DnsQueries") {
		if (DnsCache.find(mc.Value) != DnsCache.end()) {
			syslog(LOG_DEBUG, "Found DnsQuery for %s from host %s", mc.Value.c_str(), MacAddress.c_str());
			return true;
		} else {
			syslog(LOG_DEBUG, "Didn't find DnsQuery for %s from host %s", mc.Value.c_str(), MacAddress.c_str());
		}
	} else {
		syslog(LOG_DEBUG, "Unsupported MustContain key %s", mc.Key.c_str());
	}
	return false;
}

bool Host::DeviceStats(json& j, uint32_t time_interval, bool force, bool detailed) {
	// Don't report info if device has been matched
	// or if device hasn't been modified since last reporting run
	if (!force && (isMatched() || LastModified < (time(nullptr) - time_interval))) {
		return false;
	}
	j["MacOid"] = MacAddress.substr(0,8);
	j["DhcpHostname"] = Dhcp.DhcpHostname;
	j["DhcpVendor"] = Dhcp.DhcpVendor;
	j["Hostname"] = Dhcp.Hostname;
	j["SsdpFriendlyName"] = Ssdp.FriendlyName;
	j["SsdpManufacturer"] = Ssdp.Manufacturer;
	j["SsdpManufacturerUrl"] = Ssdp.ManufacturerUrl;
	j["SsdpModelName"] = Ssdp.ModelName;
	j["SsdpModelUrl"] = Ssdp.ModelUrl;
	j["SsdpSerialNumber"] = Ssdp.SerialNumber;
	j["SsdpUserAgent"] = Ssdp.UserAgent;
	j["SsdpServer"] = Ssdp.Server;
	j["SsdpLocation"] = Ssdp.Location;

	std::string fqdns;
	for (auto &dq: DnsCache) {
		if (detailed)
			dq.second->DnsStats(j, time_interval);
		else
			fqdns += dq.second->Fqdn_get() + " ";
	}
	if (! detailed)
		j["DnsQueries"] = fqdns;

	return true;
}

bool Host::TrafficStats(json& j, const uint32_t time_interval, bool force) {
	if (!force && (!isMatched() || LastModified < (time(nullptr) - time_interval))) {
		return false;
	}
	std::unordered_set<std::string> allIps;
	std::unordered_set<std::string> allFqdns;
	j = { {"DeviceProfileUuid", Uuid } };
	j["DnsQueries"] = json::array();
	for (auto &dq: DnsCache) {
		j["DnsQueries"].push_back(dq.first);
		dq.second->Ips_get(allIps);
	}

	j["TrafficStats"] = json::array();
	for (auto &fc: FlowCache) {
		std::string ip = fc.first;
		if (allIps.find(ip) == allIps.end()) {
			j["TrafficStats"].push_back(ip);
			allIps.insert(ip);
		}
	}
	return true;
}

void Host::ExportDeviceInfo (json &j, bool detailed) {
	json h{{"MacAddress", MacAddress},
			{"DeviceProfileUuid", Uuid},
			{"Ipv4Address", Ipv4Address},
			{"SsdpManufacturer", Ssdp.Manufacturer},
			{"SsdpModelName", Ssdp.ModelName}
	};
	if (detailed) {
		DeviceStats(h, 604800, true);
	}
	j.push_back(h);
}

/*
 *  Host::FlowEntry_set
 *  Adds or updates the list of flows from a host to a remote host
 *  Returns true if flow was added, false if existing flow was updated
 */
bool Host::FlowEntry_set(const uint16_t inSrcPort, const std::string &inDstIp, const uint16_t inDstPort, const uint8_t inProtocol, const uint32_t inExpiration) {
	iCache::LastSeen = iCache::LastModified = time(nullptr);
	auto f = std::make_shared<FlowEntry>();
	syslog(LOG_DEBUG, "Creating new Flow Entry for src port %u, dest ip %s, dest port %u, protocol %u", inSrcPort, inDstIp.c_str(), inDstPort, inProtocol);
	f->SrcPort = inSrcPort;
	f->DstPort = inDstPort;
	f->Protocol = inProtocol;
	f->Expiration_set();
	// Is there already at least one flow from the Host to the destination IP?
	if (FlowCache.find(inDstIp) == FlowCache.end()) {
		FlowCache[inDstIp] = std::make_shared<FlowEntryList>();
		FlowCache[inDstIp]->push_back(f);
		syslog(LOG_DEBUG, "Adding to FlowCache with destination %s : %u Protocol %u", inDstIp.c_str(), inDstPort, inProtocol);
		return true;
	}
	// Create or update existing flow to destination IP
	for(FlowEntryList::iterator existingflow = FlowCache[inDstIp]->begin(); existingflow != FlowCache[inDstIp]->end(); ++existingflow) {
		// Update existing flow it it matches incoming flow (ignoring Expiration)
		if (**existingflow == *f) {
			syslog(LOG_DEBUG, "Updating expiration of existing FlowEntry in FlowCache for destination %s", inDstIp.c_str());
			(*existingflow)->Expiration_set(inExpiration);
			return false;
		}
	}
	// This flow doesn't match any of the existing flows
	syslog(LOG_DEBUG, "Adding FlowEntry to FlowCache for destination %s", inDstIp.c_str());
	FlowCache[inDstIp]->push_back(f);
	return true;
}

/*
 *  Host::DnsLogwEntry_set
 *  Adds or updates the list of dns log entries for lookups of a FQDN
 *  Returns true if a DnsLogEntry was added, false if existing DnsLogEntry was updated
 */
bool Host::DnsLogEntry_set(const std::string inFqdn, const std::string inIpAddress, const uint32_t inExpiration) {
	iCache::LastSeen = time(nullptr);
	bool newentry = false;
	if(DnsCache.find(inFqdn) == DnsCache.end()) {
		DnsCache[inFqdn] = std::make_shared<DnsLogEntry>(inFqdn);
		newentry = true;
		syslog(LOG_DEBUG, "Creating DnsLogEntry for %s", inFqdn.c_str());
	}

	if (DnsCache[inFqdn]->Ips_set(inIpAddress, inExpiration))
		iCache::LastModified = iCache::LastSeen;

	return newentry;
}

bool Host::Dhcp_set (const std::shared_ptr<DhcpRequest> inDhcp_sptr) {
	iCache::LastSeen = time(nullptr);
	if (Dhcp == *(inDhcp_sptr))
		return false;

	iCache::FirstSeen = iCache::LastModified = iCache::LastSeen = time(nullptr);

	Dhcp = *inDhcp_sptr;
	Dhcp.Expiration_set();
	return true;
}

bool Host::Dhcp_set (const DhcpRequest &inDhcpRequest) {
	iCache::LastSeen = time(nullptr);
	if (Dhcp == inDhcpRequest)
		return false;

	iCache::FirstSeen = iCache::LastModified = iCache::LastSeen = time(nullptr);

	Dhcp = inDhcpRequest;
	Dhcp.Expiration_set();
	return true;
}

bool Host::Dhcp_set (const std::string IpAddress, const std::string MacAddress, const std::string Hostname, const std::string DhcpHostname, const std::string DhcpVendor) {
	Dhcp.IpAddress = IpAddress;
	Dhcp.MacAddress = MacAddress;
	Dhcp.Hostname = Hostname;
	Dhcp.DhcpHostname = DhcpHostname;
	Dhcp.DhcpVendor = DhcpVendor;

	iCache::FirstSeen = iCache::LastModified = iCache::LastSeen = time(nullptr);
	Dhcp.Expiration_set();
	return true;
}

bool Host::SsdpInfo_set(const std::shared_ptr<SsdpHost> insHost) {
	iCache::LastSeen = time(nullptr);
	if (Ssdp == *insHost)
		return false;

	iCache::LastModified = iCache::LastSeen;
	Ssdp = *insHost;

	// Information in the SSDP multicast message has changed so if the Location field contains are URL, we query it
	if (Ssdp.Location != "") {
		auto resp = SsdpLocation::Get(Ssdp);
	}
	return true;
}

uint32_t Host::Prune (bool Force) {
	bool pruned = false;
	syslog(LOG_DEBUG, "Pruning host %s", MacAddress.c_str());
	uint32_t pruned_flowentries = 0;
	uint32_t pruned_flows = 0;
	// FlowCache is a map, so iterate over it
	auto fc = FlowCache.begin();
	while (fc != FlowCache.end()) {
		std::string destip = fc->first;
		// fc is an iterator to pair{std::string,shared_ptr<FlowEntryList>}
		// fc.second a shared_ptr<FlowEntryList>
		// *(fc.second) is a FlowEntryList
		// fel is a reference to FlowEntryList
		auto & fel = *(fc->second);
		// FlowEntryList is std::list<std::shared_ptr<FlowEntry>>
		auto it = fel.begin();
		while (it != fel.end()) {
			if (Force || (*it)->isExpired()) {
				// Remove element from list
				it = fel.erase(it);
				pruned_flowentries++;
				pruned = true;
			} else {
				++it;
			}
		}
		// If the list of Flow Entry pointers is empty, delete it
		if (Force || fel.empty()) {
			// delete(&fel);
			fc = FlowCache.erase(fc);
			pruned = true;
			pruned_flows++;
		} else {
			++fc;
		}

	}
	uint32_t pruned_dnsqueries = 0;
	syslog (LOG_DEBUG, "Pruned %u Flow Entries and %u flows", pruned_flowentries, pruned_flows);
	for(auto const& dc: DnsCache) {
		if (Force || dc.second->isExpired()) {
			DnsCache.erase(dc.first);
			pruned_dnsqueries++;
			pruned = true;
		}
	}
	syslog (LOG_DEBUG, "Pruned %u DNS queries", pruned_dnsqueries);
	return pruned;
}

