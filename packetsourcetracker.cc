/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include <string>
#include <sstream>

#include <unistd.h>
#include <sys/types.h>

#include "util.h"
#include "packetsourcetracker.h"
#include "packetsource.h"
#include "configfile.h"
#include "getopt.h"

#ifdef SYS_LINUX
// Bring in the ifcontrol stuff for 'auto'
#include "ifcontrol.h"
#endif

enum CARD_fields {
	CARD_interface, CARD_type, CARD_username, CARD_channel, CARD_uuid, 
	CARD_packets, CARD_hop, CARD_velocity, CARD_dwell, CARD_hop_tv_sec,
	CARD_hop_tv_usec, CARD_channellist,
	CARD_maxfield
};

const char *CARD_fields_text[] = {
	"interface", "type", "username", "channel", "uuid", "packets", "hop",
	"velocity", "dwell", "hop_time_sec", "hop_time_usec", "channellist",
	NULL
};

enum PROTOSOURCE_fields {
	PROTOSOURCE_type, PROTOSOURCE_root, PROTOSOURCE_defaultset,
	PROTOSOURCE_maxfield
};

const char *PROTOSOURCE_fields_text[] = {
	"type", "root", "defaultset",
	NULL
};

// IPC hooks
int pst_ipc_add_source(IPC_CMD_PARMS) {
	if (parent) return 0;

	if (len < (int) sizeof(ipc_source_add))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcAddPacketsource((ipc_source_add *) data);

	return 1;
}

int pst_ipc_add_channellist(IPC_CMD_PARMS) {
	if (parent) return 0;

	if (len < (int) sizeof(ipc_source_add_chanlist))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcAddChannelList(
							(ipc_source_add_chanlist *) data);

	return 1;
}

int pst_ipc_set_channel(IPC_CMD_PARMS) {
	if (parent) return 0;

	if (len < (int) sizeof(ipc_source_chanset))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcChannelSet((ipc_source_chanset *) data);

	return 1;
}

int pst_ipc_sync_complete(IPC_CMD_PARMS) {
	if (parent) return 0;

	((Packetsourcetracker *) auxptr)->RegisterIPC(NULL, 0);

	return 1;
}

// Server rx of report statistics
int pst_ipc_rx_stats(IPC_CMD_PARMS) {
	if (!parent) return 0;

	if (len < (int) sizeof(ipc_source_report))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcSourceReport((ipc_source_report *) data);

	return 1;
}

int pst_ipc_run(IPC_CMD_PARMS) {
	if (parent) return 0;

	if (len < (int) sizeof(ipc_source_run))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcSourceRun((ipc_source_run *) data);

	return 1;
}

int pst_ipc_remove(IPC_CMD_PARMS) {
	if (parent) return 0;

	if (len < (int) sizeof(ipc_source_remove))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcSourceRemove((ipc_source_remove *) data);

	return 1;
}

int pst_ipc_packet(IPC_CMD_PARMS) {
	if (!parent) return 0;

	if (len < (int) sizeof(ipc_source_packet))
		return 0;

	((Packetsourcetracker *) auxptr)->IpcPacket((ipc_source_packet *) data);

	return 1;
}

int pst_channeltimer(TIMEEVENT_PARMS) {
	((Packetsourcetracker *) parm)->ChannelTimer();

	return 1;
}

int pst_chain_hook(CHAINCALL_PARMS) {
	((Packetsourcetracker *) auxdata)->ChainHandler(in_pack);
	return 1;
}

void Packetsourcetracker::Usage(char *name) {
	printf(" *** Packet Capture Source Options ***\n");
	printf(" -c, --capture-source         Specify a new packet capture source\n"
		   "                              (Identical syntax to the config file)\n"
		   " -C, --enable-capture-sources Enable capture sources (comma-separated\n"
		   "                              list of names or interfaces)\n");
}

Packetsourcetracker::Packetsourcetracker(GlobalRegistry *in_globalreg) {
    globalreg = in_globalreg;

	if (globalreg->kisnetserver == NULL) {
		fprintf(stderr, "FATAL OOPS:  Packetsourcetracker called before "
				"kisnetframework\n");
		exit(1);
	} 

	if (globalreg->timetracker == NULL) {
		fprintf(stderr, "FATAL OOPS:  Packetsourcetracker called before "
				"timetracker\n");
		exit(1);
	}

	// Register packet components on the server-side code
	if (globalreg->packetchain != NULL) {
		// Register our packet components 
		// back-refer to the capsource so we can get names and parameters
		_PCM(PACK_COMP_KISCAPSRC) =
			globalreg->packetchain->RegisterPacketComponent("KISCAPSRC");
		// Basic packet chunks everyone needs - while it doesn't necessarily
		// make sense to do this here, it makes as much sense as anywhere else
		_PCM(PACK_COMP_RADIODATA) =
			globalreg->packetchain->RegisterPacketComponent("RADIODATA");
		_PCM(PACK_COMP_LINKFRAME) =
			globalreg->packetchain->RegisterPacketComponent("LINKFRAME");
		_PCM(PACK_COMP_80211FRAME) =
			globalreg->packetchain->RegisterPacketComponent("80211FRAME");
		_PCM(PACK_COMP_FCSBYTES) =
			globalreg->packetchain->RegisterPacketComponent("FCSBYTES");

		globalreg->packetchain->RegisterHandler(&pst_chain_hook, this,
												CHAINPOS_POSTCAP, -100);
	}

	// Register the packetsourcetracker as a pollable subsystem
	globalreg->RegisterPollableSubsys(this);

	// Set up the base source IDs - 0 indicates error
	next_source_id = 1;
	next_channel_id = 1;

	running_as_ipc = 0;
	rootipc = NULL;

	channel_time_id = 
		globalreg->timetracker->RegisterTimer(1, NULL, 1, &pst_channeltimer, this);
}

Packetsourcetracker::~Packetsourcetracker() {
	globalreg->RemovePollableSubsys(this);

	globalreg->timetracker->RemoveTimer(channel_time_id);

	if (globalreg->packetchain != NULL)
		globalreg->packetchain->RemoveHandler(&pst_chain_hook, CHAINPOS_POSTCAP);

	// We could delete the card stuff but we're only ever called during
	// shutdown and fail, so who cares.
}

void Packetsourcetracker::RegisterIPC(IPCRemote *in_ipc, int in_as_ipc) {
	// Ignore params if we're called with NULL
	if (in_ipc != NULL) {
		rootipc = in_ipc;
		running_as_ipc = in_as_ipc;
		sync_ipc_id = 
			rootipc->RegisterIPCCmd(*pst_ipc_sync_complete, NULL, this, "SYNCCOMPLETE");
	}

	// Register on both sides of the IPC so the negotiation works properly
	source_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_add_source, NULL, this, "SOURCEADD");
	channellist_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_add_channellist, NULL, this, "SOURCEADDCHAN");
	channel_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_set_channel, NULL, this, "SOURCESETCHAN");
	report_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_rx_stats, NULL, this, "SOURCEREPORT");
	run_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_run, NULL, this, "SOURCERUN");
	packet_ipc_id =
		rootipc->RegisterIPCCmd(&pst_ipc_packet, NULL, this, "SOURCEFRAME");
}

// Simple aggregate of all our pollable sources.  Sources linked via IPC will
// be ignored (they'll return -1 for the descriptor)
unsigned int Packetsourcetracker::MergeSet(unsigned int in_max_fd, 
										   fd_set *out_rset, fd_set *out_wset) {
	// don't merge during shutdown
	if (globalreg->spindown)
		return in_max_fd;

	unsigned int max = in_max_fd;

	for (map<uint16_t, pst_packetsource *>::const_iterator x = packetsource_map.begin();
		 x != packetsource_map.end(); ++x) {
		if (x->second->strong_source == NULL || x->second->error == 1)
			continue;

		int capd = x->second->strong_source->FetchDescriptor();

		if (capd < 0)
			continue;

		FD_SET(capd, out_rset);
		if (capd > (int) max)
			max = capd;
	}

	return max;
}

// Kick a packet source poll event
int Packetsourcetracker::Poll(fd_set& in_rset, fd_set& in_wset) {
	if (globalreg->spindown)
		return 0;

	for (map<uint16_t, pst_packetsource *>::iterator x = packetsource_map.begin();
		 x != packetsource_map.end(); ++x) {
		if (x->second->strong_source == NULL)
			continue;

		int capd = x->second->strong_source->FetchDescriptor();

		if (capd >= 0 && FD_ISSET(capd, &in_rset))
			x->second->strong_source->Poll();
	}

	return 1;
}

int Packetsourcetracker::RegisterPacketSource(KisPacketSource *in_weak) {
	return in_weak->RegisterSources(this);
}

int Packetsourcetracker::RegisterPacketProto(string in_type, KisPacketSource *in_weak,
											 string in_defaultchan, int in_root) {
	pst_protosource *proto;

	for (unsigned int x = 0; x != protosource_vec.size(); x++) {
		if (protosource_vec[x]->type == StrLower(in_type)) {
			_MSG("Packet source type '" + in_type + "' already registered, ignoring.",
				 MSGFLAG_ERROR);
			return 0;
		}
	}

	proto = new pst_protosource;

	proto->type = in_type;
	proto->weak_source = in_weak;
	proto->default_channelset = in_defaultchan;
	proto->require_root = in_root;

	protosource_vec.push_back(proto);

	return 1;
}

uint16_t Packetsourcetracker::AddChannelList(string in_chanlist) {
	vector<string> cvec;
	vector<string> tvec;
	string name;
	size_t pos = in_chanlist.find(":");
	pst_channellist *chlist;
	vector<pst_channel> chvec;
	pst_channel ch;

	if (in_chanlist.length() == 0 || pos == string::npos) {
		_MSG("Invalid channel list, expected 'channellist=<name>:{<ch>[:<dwell>],}+'",
			 MSGFLAG_ERROR);
		return 0;
	}

	name = in_chanlist.substr(0, pos);
	cvec = StrTokenize(in_chanlist.substr(pos + 1, in_chanlist.size() - pos - 1), ",");

	if (cvec.size() == 0) {
		_MSG("Invalid channel list, expected 'channellist=<name>:{<ch>[:<dwell>],}+'",
			 MSGFLAG_ERROR);
		return 0;
	}

	for (unsigned int x = 0; x < cvec.size(); x++) {
		ch.channel = 0;
		ch.dwell = 1;

		tvec = StrTokenize(cvec[x], ":");
		
		if (tvec.size() >= 1) {
			if (sscanf(tvec[0].c_str(), "%u", &ch.channel) != 1) {
				_MSG("Invalid channel in channel list '" + name + "', expected "
					 "channel number or mhz frequency.", MSGFLAG_ERROR);
				return 0;
			}
		}

		if (tvec.size() >= 2) {
			if (sscanf(tvec[1].c_str(), "%u", &ch.dwell) != 1) {
				_MSG("Invalid dwell time in channel list '" + name + "', expected "
					 "a dwell time as a number.", MSGFLAG_ERROR);
				return 0;
			}
		}

		if (ch.dwell > 5) {
			_MSG("Dwell time in channel list '" + name + "' is over 5 periods, "
				 "this might indicate a typo in the channel config as it is longer "
				 "than expected.", MSGFLAG_ERROR);
		}

		chvec.push_back(ch);
	}

	chlist = new pst_channellist;
	chlist->channel_id = next_channel_id;
	chlist->name = StrLower(name);
	chlist->channel_vec = chvec;

	next_channel_id++;
	channellist_map[chlist->channel_id] = chlist;

	SendIPCChannellist(chlist);

	return chlist->channel_id;
}

uint16_t Packetsourcetracker::AddPacketSource(string in_source, 
											  KisPacketSource *in_strong) {
	string interface;
	string type;
	string chanlistname;
	vector<opt_pair> options;
	size_t pos = in_source.find(":");
	pst_packetsource *pstsource = NULL;
	int found = 0;

	if (pos == string::npos) {
		interface = in_source;
	} else {
		interface = in_source.substr(0, pos);
		if (StringToOpts(in_source.substr(pos + 1, in_source.size() - pos - 1), ",",
						 &options) < 0) {
			_MSG("Invalid options list for source '" + interface + "', expected "
				 "ncsource=interface[,option=value]+", MSGFLAG_ERROR);
			return 0;
		}
	}

	pstsource = new pst_packetsource;
	if (in_strong != NULL)
		pstsource->local_only = 1;
	else
		pstsource->local_only = 0;
	pstsource->sourceline = in_source;
	pstsource->strong_source = in_strong;
	pstsource->proto_source = NULL;
	// Set to an undefined status first
	pstsource->channel = 0;
	pstsource->channel_list = 0;
	pstsource->channel_ptr = NULL;
	pstsource->channel_hop = -1;
	pstsource->channel_position = 0;
	pstsource->channel_dwell = -1;
	pstsource->channel_rate = -1;
	pstsource->channel_split = 1;

	pstsource->rate_timer = 0;
	pstsource->dwell_timer = 0;

	pstsource->tm_hop_start.tv_sec = 0;
	pstsource->tm_hop_start.tv_usec = 0;
	pstsource->tm_hop_time.tv_sec = 0;
	pstsource->tm_hop_time.tv_usec = 0;

	pstsource->consec_channel_err = 0;

	pstsource->error = 0;

	// Try to map the type when they tell us what it is
	if ((type = FetchOpt("type", &options)) != "" && type != "auto" &&
		pstsource->strong_source == NULL) {
		found = 0;

		for (unsigned int x = 0; x < protosource_vec.size(); x++) {
			if (protosource_vec[x]->type == StrLower(type)) {
				found = 1;
				pstsource->proto_source = protosource_vec[x];
				break;
			}
		}

		if (found == 0) {
			_MSG("Invalid type '" + type + "'; Unknown, or support was not compiled "
				 "into this build of Kismet, check the output of the 'configure' "
				 "script if you compiled Kismet yourself.", MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}
	} 

	// Try to figure out the auto types
	if (pstsource->strong_source == NULL && (type == "auto" || type == "")) {
		for (unsigned int x = 0; x < protosource_vec.size(); x++) {
			if (protosource_vec[x]->weak_source->AutotypeProbe(interface) != 0) {
				pstsource->proto_source = protosource_vec[x];
				type = pstsource->proto_source->weak_source->FetchType();
				_MSG("Matched source type '" + type + "' for auto-type source '" + 
					 interface + "'", MSGFLAG_INFO);
				ReplaceAllOpts("type", type, &options);
				break;
			}
		}

		if (type == "" || type == "auto") {
			_MSG("Failed to find a type for auto-type source '" + interface + "', "
				 "you will have to tell Kismet what it is by adding a "
				 "type=[card type] to the ncsource config", MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}
	}

	// Push the option set if we were given a strong source
	if (in_strong != NULL)
		pstsource->strong_source->ParseOptions(&options);

	// Resolve the channel list
	chanlistname = FetchOpt("channellist", &options);

	if (chanlistname == "")  {
		chanlistname = pstsource->proto_source->default_channelset;
		_MSG("Using default channel list '" + chanlistname + "' on source '" +
			 interface + "'", MSGFLAG_INFO);
	} else {
		_MSG("Using channel list '" + chanlistname + "' on source '" + 
			 interface + "' instead of the default", MSGFLAG_INFO);
	}

	found = 0;
	for (map<uint16_t, pst_channellist *>::iterator x = channellist_map.begin();
		 x != channellist_map.end(); ++x) {
		if (StrLower(chanlistname) == x->second->name) {
			found = 1;
			pstsource->channel_list = x->first;
			pstsource->channel_ptr = x->second;
			break;
		}
	}

	if (found == 0 && chanlistname != "n/a") {
		_MSG("Missing channel list '" + chanlistname + "' for source '" +
			 interface + "'.  Make sure your kismet.conf file contains a "
			 "channellist=" + chanlistname + " line", MSGFLAG_ERROR);
		return 0;
	}

	// Do the initial build of a strong source now that we know the type
	pstsource->strong_source = 
		pstsource->proto_source->weak_source->CreateSource(globalreg, interface, 
														   &options);

	// Figure out stuff we need the source definition for, after we've errored out
	// on the common failures
	
	if (pstsource->strong_source->FetchChannelCapable() == 0) {
		_MSG("Disabling channel hopping on source '" + interface + "' because "
			 "it is not capable of setting the channel.", MSGFLAG_INFO);
		pstsource->channel_hop = 0;

	} else {
		pstsource->channel_hop = 1;

		if (FetchOpt("channel", &options) != "") {
			_MSG("Source '" + interface + "' ignoring channel= in the source "
				 "options because it is set to hop, specify hop=false to lock "
				 "to a specific channel.", MSGFLAG_INFO);
		}
	}

	if (FetchOpt("hop", &options) != "true" && FetchOpt("hop", &options) != "") {
		_MSG("Disabling channel hopping on source '" + interface + "' because the "
			 "source options include hop=false", MSGFLAG_INFO);
		pstsource->channel_hop = 0;

		if (FetchOpt("channel", &options) == "") {
			_MSG("Source '" + interface + "' has no no channel= in the source "
				 "options but has channel hopping disabled, specify a channel",
				 MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}

		if (sscanf(FetchOpt("channel", &options).c_str(), "%hu", 
				   &(pstsource->channel)) != 1) {
			_MSG("Invalid channel for source '" + interface + "', expected "
				 "channel number or frequency", MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}

		_MSG("Source '" + interface + "' will be locked to channel " +
			 FetchOpt("channel", &options), MSGFLAG_INFO);

	} 
	
	if (FetchOpt("hop", &options) == "" && FetchOpt("channel", &options) != "" &&
			   pstsource->channel_hop != 0) {
		_MSG("Ignoring channel= option for source '" + interface + "' because "
			 "the source is channel hopping.  Set hop=false on the source options "
			 "to disable hopping and lock to the specified channel", MSGFLAG_INFO);
	}

	if (FetchOpt("dwell", &options) != "" && pstsource->channel_hop) {
		if (sscanf(FetchOpt("dwell", &options).c_str(), "%d", 
				   &pstsource->channel_dwell) != 1) {
			_MSG("Invalid time for source '" + interface + "' dwell time, expected "
				 "time in seconds to dwell on a channel", MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}

		_MSG("Source '" + interface + "' will dwell on each channel " +
			 FetchOpt("dwell", &options) + " second(s)", MSGFLAG_INFO);
	}

	if (FetchOpt("velocity", &options) != "" && pstsource->channel_hop) {
		if (sscanf(FetchOpt("velocity", &options).c_str(), "%d", 
				   &pstsource->channel_rate) != 1) {
			_MSG("Invalid time for source '" + interface + "' hop rate, expected "
				 "velocity in channels per second to (attempt) hopping", MSGFLAG_ERROR);
			delete pstsource;
			return 0;
		}

		if (pstsource->channel_dwell > 0) {
			_MSG("Conflicting options for source '" + interface + "', cannot specify "
				 "both dwell (seconds spend on channel) and rate (channels per second "
				 "hop rate) for the same source, dwell will be ignored and hop rate "
				 "will be used.", MSGFLAG_ERROR);
			pstsource->channel_dwell = 0;
		}

		if (pstsource->channel_rate > SERVER_TIMESLICES_SEC) {
			ostringstream osstr;

			osstr << "Channel rate for source '" << interface << "' specified as " <<
				pstsource->channel_rate << " but Kismet only allows a maximum of " <<
				SERVER_TIMESLICES_SEC << " channel hops per second, packet rate " 
				"will be fixed to the maximum supported rate.";

			_MSG(osstr.str(), MSGFLAG_ERROR);

			pstsource->channel_rate = SERVER_TIMESLICES_SEC;
		}

		_MSG("Source '" + interface + "' will attempt to hop at " +
			 FetchOpt("velocity", &options) + " channel(s) per second.", MSGFLAG_INFO);
	}

	// Assume the defaults
	if (pstsource->channel_dwell < 0)
		pstsource->channel_dwell = default_channel_dwell;
	if (pstsource->channel_rate < 0)
		pstsource->channel_rate = default_channel_rate;

	if (FetchOpt("split", &options) != "" && pstsource->channel_hop) {
		if (FetchOpt("split", &options) != "true") {
			_MSG("Disabling channel list splitting on interface '" + interface + "' "
				 "because split=false was in the source options.  This source will "
				 "not balance channel offsets with other sources using the same "
				 "channel list and will instead hop normally", MSGFLAG_INFO);
			pstsource->channel_split = 0;
		}
	}

	// Add the created strong source to our list
	pstsource->source_id = next_source_id;
	packetsource_map[pstsource->source_id] = pstsource;
	packetsource_vec.push_back(pstsource);

	next_source_id++;

	SendIPCSourceAdd(pstsource);

	// Send a notify to all the registered callbacks
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		(*(cb_vec[x]->cb))(globalreg, pstsource, SOURCEACT_ADDSOURCE, 0, 
						   cb_vec[x]->auxdata);
	}

	return pstsource->source_id;
}

int Packetsourcetracker::LoadConfiguration() {
	vector<string> src_input_vec;
	vector<string> chan_vec;
	string named_sources;

	int option_idx = 0;

	static struct option packetsource_long_options[] = {
		{ "capture-source", required_argument, 0, 'c' },
		{ "enable-capture-source", required_argument, 0, 'C' },
		{ 0, 0, 0, 0 }
	};

	default_channel_rate = 5;
	default_channel_dwell = 0;

	if (globalreg->kismet_config == NULL) {
		_MSG("Packetsourcetracker LoadConfiguration called before kismet_config "
			 "loaded", MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	}

	// Parse the default velocity and dwell
	if (globalreg->kismet_config->FetchOpt("channelvelocity") != "" &&
		sscanf(globalreg->kismet_config->FetchOpt("channelvelocity").c_str(),
			   "%d", &default_channel_rate) != 1) {
		_MSG("Invalid channelvelocity=... in the Kismet config file, expected "
			 "a number of channels per second to attempt to hop.", MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	} 

	if (globalreg->kismet_config->FetchOpt("channeldwell") != "" &&
		sscanf(globalreg->kismet_config->FetchOpt("channeldwell").c_str(),
			   "%d", &default_channel_rate) != 1) {
		_MSG("Invalid channeldwell=... in the Kismet config file, expected "
			 "a number of seconds per channel to wait", MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	} 

	if (default_channel_dwell != 0) {
		_MSG("Kismet will dwell on each channel for " +
			 globalreg->kismet_config->FetchOpt("channeldwell") + " seconds "
			 "unless overridden by source-specific options.", MSGFLAG_INFO);
	} else if (default_channel_rate != 0) {
		_MSG("Kismet will attempt to hop channels at " +
			 globalreg->kismet_config->FetchOpt("channelvelocity") + " channels "
			 "per second unless overridden by source-specific options", MSGFLAG_INFO);
	} else {
		_MSG("No default channel dwell or hop rates specified, Kismet will attempt "
			 "to hop 5 channels per second.", MSGFLAG_INFO);
		default_channel_rate = 5;
	}

	// Hack the extern getopt index
	optind = 0;

	while (1) {
		int r = getopt_long(globalreg->argc, globalreg->argv, "-c:C:",
							packetsource_long_options, &option_idx);

		if (r < 0) break;

		switch (r) {
			case 'C':
				named_sources = string(optarg);
				break;
			case 'c':
				src_input_vec.push_back(string(optarg));
				break;
		}
	}

	// If we didn't get any command line options, pull both from the config
	// file options
	if (named_sources.length() == 0 && src_input_vec.size() == 0) {
		_MSG("No specific sources named on the command line, sources will be read "
			 "from kismet.conf", MSGFLAG_INFO);
		named_sources =
			globalreg->kismet_config->FetchOpt("enablesources");
		src_input_vec =
			globalreg->kismet_config->FetchOptVec("ncsource");
	} else if (src_input_vec.size() == 0) {
		_MSG("Reading sources from kismet.conf but only enabling sources specified "
			 "on the command line", MSGFLAG_INFO);
		src_input_vec =
			globalreg->kismet_config->FetchOptVec("ncsource");
	}

	if (src_input_vec.size() == 0) {
		_MSG("No sources found - Remember, Kismet recently changed the format of "
			 "sources, and to make it easier to find old configs, sources are now "
			 "defined by the ncsource=... config file line.  Please check the "
			 "Kismet devel blog for details and update your config file.",
			 MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return -1;
	}

	// Fetch the channel lists and add them
	chan_vec = 
		globalreg->kismet_config->FetchOptVec("channellist");

	if (chan_vec.size() == 0) {
		if (chan_vec.size() == 0) {
			_MSG("No channels found - Remember, Kismet recently changed the format of "
				 "channels, and to make it easier to find old configs, channels are now "
				 "defined by the channellist=... config file line.  Please check the "
				 "Kismet devel blog for details and update your config file.",
				 MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return -1;
		}
	}

	for (unsigned int x = 0; x < chan_vec.size(); x++) {
		if (AddChannelList(chan_vec[x]) == 0) {
			_MSG("Failed to add channel list '" + chan_vec[x] + "', check your "
				 "syntax and remember Kismet recently changed how it handles "
				 "channels!", MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return -1;
		}
	}

	// Process sources
	for (unsigned int x = 0; x < src_input_vec.size(); x++) {
		if (AddPacketSource(src_input_vec[x], NULL) == 0) {
			_MSG("Failed to add source '" + src_input_vec[x] + "', check your "
				 "syntax and remember Kismet recently changed how it handles "
				 "source definitions!", MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return -1;
		}
	}

	// Figure out our split channel assignments by mapping assigned counts,
	// then checking to see if we're sharing on any sources which have 
	// differing dwell/hop rates and warn, then assign offsets
	map<uint16_t, int> chanid_count_map;

	for (map<uint16_t, pst_packetsource *>::iterator x = packetsource_map.begin();
		 x != packetsource_map.end(); ++x) {
		if (x->second->channel_hop == 0 || x->second->channel_split == 0)
			continue;

		if (chanid_count_map.find(x->second->channel_list) == chanid_count_map.end())
			chanid_count_map[x->second->channel_list] = 0;

		chanid_count_map[x->second->channel_list]++;
	}

	// Second check for mismatched dwell, nasty multiple-search of the map
	// but we don't care since it's a short map and we only do this during
	// startup
	for (map<uint16_t, int>::iterator x = chanid_count_map.begin();
		 x != chanid_count_map.end(); ++x) {
		int chrate = -1, chdwell = -1;

		for (map<uint16_t, pst_packetsource *>::iterator y = packetsource_map.begin();
			 y != packetsource_map.end(); ++y) {
			if (y->second->channel_list != x->first || y->second->channel_hop == 0 || 
				y->second->channel_split == 0)
				continue;

			if (chrate < 0)
				chrate = y->second->channel_rate;
			if (chdwell < 0)
				chdwell = y->second->channel_dwell;

			string warntype;
			if (chrate != y->second->channel_rate)
				warntype = "hop rate";
			if (chdwell != y->second->channel_dwell)
				warntype = "dwell time";

			if (warntype != "") {
				_MSG("Mismatched " + warntype + " for source '" + 
					 y->second->strong_source->FetchInterface() + "' splitting "
					 "channel list " + channellist_map[x->first]->name + ".  "
					 "Mismatched " + warntype + " values will cause split hopping "
					 "to drift over time.", MSGFLAG_ERROR);
			}
		}
	}

	// Third pass to actually assign offsets to our metasources
	for (map<uint16_t, int>::iterator x = chanid_count_map.begin();
		 x != chanid_count_map.end(); ++x) {
		if (x->first < 2)
			continue;

		int offset = channellist_map[x->first]->channel_vec.size() / (x->second + 1);
		int offnum = 0;

		for (map<uint16_t, pst_packetsource *>::iterator y = packetsource_map.begin();
			 y != packetsource_map.end(); ++y) {
			if (y->second->channel_list != x->first || y->second->channel_hop == 0 || 
				y->second->channel_split == 0)
				continue;

			y->second->channel_position = offnum * offset;
			offnum++;

			// Push it to the IPC
			SendIPCChanset(y->second);
		}
	}

	return 1;
}

int Packetsourcetracker::IpcAddChannelList(ipc_source_add_chanlist *in_ipc) {
	pst_channellist *chlist;

	// Replace channel lists if we get the same one
	if (channellist_map.find(in_ipc->chanset_id) != channellist_map.end()) {
		chlist = channellist_map[in_ipc->chanset_id];
		
		chlist->channel_vec.clear();
	} else {
		chlist = new pst_channellist;

		channellist_map[in_ipc->chanset_id] = chlist;
	}

	for (unsigned int x = 0; 
		 x < kismin(IPC_SOURCE_MAX_CHANS, in_ipc->num_channels); x++) {
		pst_channel ch;

		ch.channel = in_ipc->chan_list[x];
		ch.dwell = in_ipc->chan_dwell_list[x];

		chlist->channel_vec.push_back(ch);
	}

	return 1;
}

int Packetsourcetracker::IpcAddPacketsource(ipc_source_add *in_ipc) {
	string interface;
	vector<opt_pair> options;
	string in_source = string(in_ipc->sourceline);
	size_t pos = in_source.find(":");
	pst_packetsource *pstsource = NULL;
	int found = 0;

	pstsource = new pst_packetsource;
	pstsource->local_only = 0;
	pstsource->sourceline = string(in_ipc->sourceline);
	pstsource->strong_source = NULL;
	pstsource->proto_source = NULL;

	// Import from the IPC packet
	pstsource->channel = in_ipc->channel;
	pstsource->channel_list = in_ipc->channel_id;
	pstsource->channel_ptr = NULL;
	pstsource->channel_hop = in_ipc->channel_hop;
	pstsource->channel_dwell = in_ipc->channel_dwell;
	pstsource->channel_rate = in_ipc->channel_rate;
	pstsource->channel_position = in_ipc->channel_position;
	pstsource->error = 0;

	pstsource->tm_hop_start.tv_sec = 0;
	pstsource->tm_hop_start.tv_usec = 0;
	pstsource->tm_hop_time.tv_sec = 0;
	pstsource->tm_hop_time.tv_usec = 0;

	pstsource->rate_timer = 0;
	pstsource->dwell_timer = 0;

	pstsource->consec_channel_err = 0;

	// We assume all our incoming data is valid but we'll check everything again
	// just to be sure
	if (pos == string::npos) {
		interface = in_source;
	} else {
		interface = in_source.substr(0, pos);
		if (StringToOpts(in_source.substr(pos + 1, in_source.size() - pos - 1), ",",
						 &options) < 0) {
			_MSG("Invalid options list for source '" + interface + "', expected "
				 "ncsource=interface[,option=value]+", MSGFLAG_ERROR);
			pstsource->error = 1;
			SendIPCReport(pstsource);
			delete pstsource;
			return -1;
		}
	}

	string type = string(in_ipc->type);

	for (unsigned int x = 0; x < protosource_vec.size(); x++) {
		if (protosource_vec[x]->type == StrLower(type)) {
			found = 1;
			pstsource->proto_source = protosource_vec[x];
			break;
		}
	}

	// These shouldn't happen but send it back as an error if it does
	if (found == 0) {
		_MSG("Invalid type '" + type + "'; Unknown, or support was not "
			 "compiled into this build of Kismet, check the output of the 'configure' "
			 "script if you compiled Kismet yourself.", MSGFLAG_ERROR);
		pstsource->error = 1;
		SendIPCReport(pstsource);
		delete pstsource;
		return -1;
	}

	// check the channel
	if (channellist_map.find(pstsource->channel_list) == channellist_map.end()) {
		_MSG("Packet source IPC got a source with an invalid channel list id, "
			 "this should never happen, check that all code sending sources sends "
			 "channel list updates first", MSGFLAG_ERROR);
		pstsource->error = 1;
		SendIPCReport(pstsource);
		delete pstsource;
		return -1;
	}

	pstsource->channel_ptr = channellist_map[pstsource->channel_list];

	// Build a strong source now that we know how, this parses any source-local
	// options in the source string that we can't pre-process.  This shouldn't
	// error since we've already passed this stage before
	pstsource->strong_source = 
		pstsource->proto_source->weak_source->CreateSource(globalreg, interface, 
														   &options);

	// All the hop/dwell/etc code is done on the server before it comes to us over IPC

	// Add the created strong source to our list
	pstsource->source_id = in_ipc->source_id;
	packetsource_map[pstsource->source_id] = pstsource;
	packetsource_vec.push_back(pstsource);

	return 1;
}

int Packetsourcetracker::IpcChannelSet(ipc_source_chanset *in_ipc) {
	pst_packetsource *pstsource = NULL;

	if (packetsource_map.find(in_ipc->source_id) == packetsource_map.end()) {
		_MSG("Packet source IPC unable to find requested source id for "
			 "channel set, something is wrong", MSGFLAG_ERROR);
		return -1;
	}

	pstsource = packetsource_map[in_ipc->source_id];

	if (in_ipc->chanset_id != 0 && 
		channellist_map.find(in_ipc->chanset_id) == channellist_map.end()) {
		_MSG("Packet source IPC unable to find requested channel id for "
			 "channel set, something is wrong", MSGFLAG_ERROR);
	}

	if (in_ipc->chanset_id == 0) {
		pstsource->channel = in_ipc->channel;
	} else {
		pstsource->channel = 0;
		pstsource->channel_list = in_ipc->chanset_id;
	}

	// Update other info
	pstsource->channel_position = 0;
	pstsource->channel_hop = in_ipc->channel_hop;
	pstsource->channel_dwell = in_ipc->channel_dwell;
	pstsource->channel_rate = in_ipc->channel;
	pstsource->channel_split = in_ipc->channel_split;

	return 1;
}

int Packetsourcetracker::IpcSourceReport(ipc_source_report *in_ipc) {
	pst_packetsource *pstsource = NULL;

	if (packetsource_map.find(in_ipc->source_id) == packetsource_map.end()) {
		_MSG("Packet source tracker unable to find source id for "
			 "report, something is wrong", MSGFLAG_ERROR);
		return -1;
	}

	pstsource = packetsource_map[in_ipc->source_id];

	// Copy the hop timestamp
	pstsource->tm_hop_time.tv_sec = in_ipc->hop_tm_sec;
	pstsource->tm_hop_time.tv_usec = in_ipc->hop_tm_usec;

	return 1;
}

int Packetsourcetracker::IpcSourceRun(ipc_source_run *in_ipc) {
	if (in_ipc->start)
		return StartSource(in_ipc->source_id);
	else
		return StopSource(in_ipc->source_id);
	
	return 1;
}

int Packetsourcetracker::IpcSourceRemove(ipc_source_remove *in_ipc) {
	pst_packetsource *pstsource = NULL;

	if (running_as_ipc == 0)
		return 0;

	if (packetsource_map.find(in_ipc->source_id) == packetsource_map.end()) {
		_MSG("Packet source tracker unable to find source id for "
			 "remove, something is wrong", MSGFLAG_ERROR);
		return -1;
	}

	pstsource = packetsource_map[in_ipc->source_id];

	return RemovePacketSource(pstsource);
}

int Packetsourcetracker::IpcPacket(ipc_source_packet *in_ipc) {
	pst_packetsource *pstsource = NULL;
	kis_packet *newpack = NULL;

	if (running_as_ipc == 1)
		return 0;

	if (packetsource_map.find(in_ipc->source_id) == packetsource_map.end()) {
		_MSG("Packet source tracker unable to find source id for "
			 "packet, something is wrong", MSGFLAG_ERROR);
		return -1;
	}

	pstsource = packetsource_map[in_ipc->source_id];

	newpack = globalreg->packetchain->GeneratePacket();

	newpack->ts.tv_sec = in_ipc->tv_sec;
	newpack->ts.tv_usec = in_ipc->tv_usec;

	kis_datachunk *linkchunk = new kis_datachunk;
	linkchunk->dlt = in_ipc->dlt;
	linkchunk->source_id = in_ipc->source_id;
	linkchunk->data = new uint8_t[in_ipc->pkt_len];
	linkchunk->length = in_ipc->pkt_len;
	memcpy(linkchunk->data, in_ipc->data, in_ipc->pkt_len);
	newpack->insert(_PCM(PACK_COMP_LINKFRAME), linkchunk);

	kis_ref_capsource *csrc_ref = new kis_ref_capsource;
	csrc_ref->ref_source = pstsource->strong_source;
	newpack->insert(_PCM(PACK_COMP_KISCAPSRC), csrc_ref);

	globalreg->packetchain->ProcessPacket(newpack);

	return 1;
}

int Packetsourcetracker::StartSource(uint16_t in_source_id) {
	uid_t euid = geteuid();
	pst_packetsource *pstsource = NULL;
	int failure = 0;

	// Start all sources.  Incrementally count failure conditions and let the caller
	// decide how to deal with them
	if (in_source_id == 0) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			if (StartSource(packetsource_vec[x]->source_id) < 0)
				failure--;
		}

		return failure;
	} 
	
	if (packetsource_map.find(in_source_id) != packetsource_map.end()) {
		pstsource = packetsource_map[in_source_id];
	} else {
		_MSG("Packetsourcetracker::StartSource called with unknown packet source "
			 "id, something is wrong.", MSGFLAG_ERROR);
		return -1;
	}

	if (euid != 0 && pstsource->proto_source->require_root && running_as_ipc) {
		_MSG("IPC child Source '" + pstsource->strong_source->FetchInterface() + 
			 "' requires root permissions to open, but we're not running "
			 "as root.  Something is wrong.", MSGFLAG_ERROR);
		return -1;
	} else if (euid != 0 && pstsource->proto_source->require_root) {
		_MSG("Deferring opening of packet source '" + 
			 pstsource->strong_source->FetchInterface() + "' to IPC child",
			 MSGFLAG_INFO);
		SendIPCStart(pstsource);
		return 0;
	}

	// Enable monitor and open it, because we're either the IPC and root, 
	// or the parent and root, or we're going to fail
	
	pstsource->strong_source->SetSourceID(pstsource->source_id);
	
	// Don't decode the DLT if we're the IPC target
	if (running_as_ipc)
		pstsource->strong_source->SetDLTMangle(0);

	if (pstsource->strong_source->EnableMonitor() < 0) {
		pstsource->error = 1;
		SendIPCReport(pstsource);
		return -1;
	}

	if (pstsource->strong_source->OpenSource() < 0) {
		pstsource->error = 1;
		SendIPCReport(pstsource);
		return -1;
	}

	SendIPCReport(pstsource);

	return 0;
}

int Packetsourcetracker::StopSource(uint16_t in_source_id) {
	// Todo - implement this
	return 0;
}

void Packetsourcetracker::SendIPCSourceAdd(pst_packetsource *in_source) {
	if (running_as_ipc == 1)
		return;

	if (rootipc == NULL)
		return;

	if (in_source == NULL) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			SendIPCSourceAdd(packetsource_vec[x]);
		}

		return;
	}

	if (in_source->local_only == 1)
		return;

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_add));
	ipc_source_add *add = (ipc_source_add *) ipc->data;

	ipc->data_len = sizeof(ipc_source_add);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = source_ipc_id;

	add->source_id = in_source->source_id;
	snprintf(add->type, 64, "%s", in_source->strong_source->FetchType().c_str());
	snprintf(add->sourceline, 4096, "%s", in_source->sourceline.c_str());
	add->channel_id = in_source->channel_list;
	add->channel = in_source->channel;
	add->channel_hop = in_source->channel_hop;
	add->channel_dwell = in_source->channel_dwell;
	add->channel_rate = in_source->channel_rate;
	add->channel_position = in_source->channel_position;

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCChannellist(pst_channellist *in_list) {
	if (running_as_ipc == 1)
		return;

	if (rootipc == NULL)
		return;

	if (in_list == NULL) {
		for (map<uint16_t, pst_channellist *>::iterator x = channellist_map.begin();
			 x != channellist_map.end(); ++x) {
			SendIPCChannellist(x->second);
		}

		return;
	}

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_add_chanlist));
	ipc_source_add_chanlist *addch = (ipc_source_add_chanlist *) ipc->data;

	ipc->data_len = sizeof(ipc_source_add_chanlist);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = channellist_ipc_id;

	addch->chanset_id = in_list->channel_id;
	addch->num_channels = in_list->channel_vec.size();

	for (unsigned int x = 0; x < kismin(IPC_SOURCE_MAX_CHANS, 
										in_list->channel_vec.size()); x++) {
		addch->chan_list[x] = in_list->channel_vec[x].channel;
		addch->chan_dwell_list[x] = in_list->channel_vec[x].dwell;
	}

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCReport(pst_packetsource *in_source) {
	if (running_as_ipc == 0)
		return;

	if (rootipc == NULL)
		return;

	if (in_source == NULL) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			SendIPCReport(packetsource_vec[x]);
		}

		return;
	}

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_report));
	ipc_source_report *report = (ipc_source_report *) ipc->data;

	ipc->data_len = sizeof(ipc_source_report);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = report_ipc_id;

	report->source_id = in_source->source_id;
	report->chanset_id = in_source->channel_list;
	// TODO figure out capabilities
	report->capabilities = 0;

	report->flags = 0;
	if (in_source->strong_source != NULL) {
		if (in_source->strong_source->FetchDescriptor() >= 0)
			report->flags |= IPC_SRCREP_FLAG_RUNNING;
	}

	if (in_source->error)
		report->flags |= IPC_SRCREP_FLAG_ERROR;

	report->hop_tm_sec = (uint32_t) in_source->tm_hop_time.tv_sec;
	report->hop_tm_usec = (uint32_t) in_source->tm_hop_time.tv_usec;

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCStart(pst_packetsource *in_source) {
	if (running_as_ipc == 1)
		return;

	if (rootipc == NULL)
		return;

	if (in_source == NULL) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			SendIPCStart(packetsource_vec[x]);
		}

		return;
	}

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_run));
	ipc_source_run *run = (ipc_source_run *) ipc->data;

	ipc->data_len = sizeof(ipc_source_run);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = run_ipc_id;

	run->source_id = in_source->source_id;
	run->start = 1;

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCChanset(pst_packetsource *in_source) {
	if (running_as_ipc == 1)
		return;

	if (rootipc == NULL)
		return;

	if (in_source == NULL) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			SendIPCChanset(packetsource_vec[x]);
		}

		return;
	}

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_chanset));
	ipc_source_chanset *chanset = (ipc_source_chanset *) ipc->data;

	ipc->data_len = sizeof(ipc_source_chanset);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = channel_ipc_id;

	chanset->source_id = in_source->source_id;
	chanset->chanset_id = in_source->channel_list;
	chanset->channel = in_source->channel;
	chanset->channel_hop = in_source->channel_hop;
	chanset->channel_dwell = in_source->channel_dwell;
	chanset->channel_rate = in_source->channel_rate;
	chanset->channel_split = in_source->channel_split;

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCRemove(pst_packetsource *in_source) {
	if (running_as_ipc == 1)
		return;

	if (rootipc == NULL)
		return;

	if (in_source == NULL) {
		for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
			SendIPCRemove(packetsource_vec[x]);
		}

		return;
	}

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_remove));
	ipc_source_remove *remove = (ipc_source_remove *) ipc->data;

	ipc->data_len = sizeof(ipc_source_remove);
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = remove_ipc_id;

	remove->source_id = in_source->source_id;

	rootipc->SendIPC(ipc);
}

void Packetsourcetracker::SendIPCPacket(kis_packet *in_pack, 
										kis_datachunk *in_linkchunk) {
	if (running_as_ipc == 0)
		return;

	if (rootipc == NULL)
		return;

	ipc_packet *ipc =
		(ipc_packet *) malloc(sizeof(ipc_packet) +
							  sizeof(ipc_source_packet) +
							  in_linkchunk->length);
	ipc_source_packet *pack = (ipc_source_packet *) ipc->data;

	ipc->data_len = sizeof(ipc_source_packet) + in_linkchunk->length;
	ipc->ipc_ack = 0;
	ipc->ipc_cmdnum = packet_ipc_id;

	pack->source_id = in_linkchunk->source_id;
	pack->tv_sec = in_pack->ts.tv_sec;
	pack->tv_usec = in_pack->ts.tv_usec;
	pack->dlt = in_linkchunk->dlt;
	pack->pkt_len = in_linkchunk->length;
	memcpy(pack->data, in_linkchunk->data, in_linkchunk->length);

	rootipc->SendIPC(ipc);
}

int Packetsourcetracker::RegisterSourceActCallback(SourceActCallback in_cb,
												   void *in_aux) {
	// Make a cb rec
	sourceactcb_rec *cbr = new sourceactcb_rec;
	cbr->cb = in_cb;
	cbr->auxdata = in_aux;

	cb_vec.push_back(cbr);

	return 1;
}

int Packetsourcetracker::RemoveSourceActCallback(SourceActCallback in_cb) {
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		if (cb_vec[x]->cb != in_cb)
			continue;

		delete cb_vec[x];
		cb_vec.erase(cb_vec.begin() + x);
		return 1;
	}

	return 0;
}

int Packetsourcetracker::SetSourceHopping(uuid in_uuid, int in_hopping, 
										  uint16_t in_channel) {
	pst_packetsource *pstsource = NULL;

	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->strong_source->FetchUUID() == in_uuid) {
			pstsource = packetsource_vec[x];
			break;
		}
	}

	if (pstsource == NULL) {
		_MSG("No packet source with UUID " + in_uuid.UUID2String() + 
			 " in change channel/hopping request", MSGFLAG_ERROR);
		return -1;
	}

	// Set the local info
	pstsource->channel_hop = in_hopping;
	pstsource->channel = in_channel;

	// Send it over IPC - we don't care if it's controlled locally
	SendIPCChanset(pstsource);

	// Send a notify to all the registered callbacks
	int opt;                                   
	if (in_hopping)                            
		opt = SOURCEACT_HOPENABLE;
	else
		opt = SOURCEACT_HOPDISABLE;
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		(*(cb_vec[x]->cb))(globalreg, pstsource, opt, 0, cb_vec[x]->auxdata);
	}

	return 1;
}

int Packetsourcetracker::SetSourceNewChannellist(uuid in_uuid, string in_channellist) {
	pst_packetsource *pstsource = NULL;

	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->strong_source->FetchUUID() == in_uuid) {
			pstsource = packetsource_vec[x];
			break;
		}
	}

	if (pstsource == NULL) {
		_MSG("No packet source with UUID " + in_uuid.UUID2String() + 
			 " to change channel list", MSGFLAG_ERROR);
		return -1;
	}

	// Parse the new channel list
	uint16_t new_id = AddChannelList(in_channellist);
	if (new_id == 0) {
		_MSG("Failed to change source '" + pstsource->strong_source->FetchInterface() +
			 "' UUID " + in_uuid.UUID2String().c_str() + " channel list because "
			 "the provided channel list definition is not valid", MSGFLAG_ERROR);
		return -1;
	}

	// Set the source up
	pstsource->channel_list = new_id;
	pstsource->channel_position = 0;

	// Send the channel update to switch us to the new list
	SendIPCChanset(pstsource);

	// Send a notify to all the registered callbacks
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		(*(cb_vec[x]->cb))(globalreg, pstsource, SOURCEACT_CHVECTOR, 
						   0, cb_vec[x]->auxdata);
	}

	return 1;
}

int Packetsourcetracker::SetSourceHopDwell(uuid in_uuid, int in_rate, int in_dwell) {
	pst_packetsource *pstsource = NULL;

	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->strong_source->FetchUUID() == in_uuid) {
			pstsource = packetsource_vec[x];
			break;
		}
	}

	if (pstsource == NULL) {
		_MSG("No packet source with UUID " + in_uuid.UUID2String() + 
			 " in change hop/dwell request", MSGFLAG_ERROR);
		return -1;
	}

	// Set the local info
	pstsource->channel_rate = in_rate;
	pstsource->channel_dwell = in_dwell;

	// Send it over IPC - we don't care if it's controlled locally
	SendIPCChanset(pstsource);

	// Send a notify to all the registered callbacks
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		(*(cb_vec[x]->cb))(globalreg, pstsource, SOURCEACT_CHHOPDWELL, 
						   0, cb_vec[x]->auxdata);
	}


	return 1;
}

int Packetsourcetracker::AddLivePacketSource(string in_source, 
											 KisPacketSource *in_strong) {
	uint8_t new_id = 0;

	if ((new_id = AddPacketSource(in_source, in_strong)) == 0) {
		return -1;
	}

	StartSource(new_id);

	return 1;
}

pst_packetsource *Packetsourcetracker::FindLivePacketSource(KisPacketSource *in_strong) {
	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->strong_source == in_strong)
			return packetsource_vec[x];
	}

	return NULL;
}

pst_packetsource *Packetsourcetracker::FindLivePacketSourceUUID(uuid in_uuid) {
	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->strong_source == NULL)
			continue;

		if (packetsource_vec[x]->strong_source->FetchUUID() == in_uuid)
			return packetsource_vec[x];
	}

	return NULL;
}

int Packetsourcetracker::RemoveLivePacketSource(KisPacketSource *in_strong) {
	pst_packetsource *pstsource = FindLivePacketSource(in_strong);

	if (pstsource != NULL)
		return RemovePacketSource(pstsource);

	return 0;
}

int Packetsourcetracker::RemovePacketSource(pst_packetsource *in_source) {
	if (in_source == NULL)
		return 0;

	packetsource_map.erase(packetsource_map.find(in_source->source_id));
	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		if (packetsource_vec[x]->source_id == in_source->source_id) {
			packetsource_vec.erase(packetsource_vec.begin() + x);
			break;
		}
	}

	if (in_source->error == 0) {
		in_source->strong_source->CloseSource();
	}

	SendIPCRemove(in_source);

	// Send a notify to all the registered callbacks
	for (unsigned int x = 0; x < cb_vec.size(); x++) {
		(*(cb_vec[x]->cb))(globalreg, in_source, SOURCEACT_DELSOURCE, 0, 
						   cb_vec[x]->auxdata);
	}

	delete in_source;

	return 1;
}

pst_channellist *Packetsourcetracker::FetchSourceChannelList(pst_packetsource *in_src) {
	if (channellist_map.find(in_src->channel_list) == channellist_map.end())
		return NULL;

	return channellist_map[in_src->channel_list];
}

void Packetsourcetracker::ChannelTimer() {
	for (unsigned int x = 0; x < packetsource_vec.size(); x++) {
		pst_packetsource *pst = packetsource_vec[x];

		if (pst->strong_source == NULL || pst->channel_ptr == NULL ||
			(pst->channel_hop == 0 && pst->channel_dwell == 0))
			continue;

		// Hop sources we have open.  Non-hoppable sources won't be set to hop
		if (pst->strong_source->FetchDescriptor() >= 0) {
			struct timeval tv;
			int push_report = 0;

			if (pst->channel_hop) {
				pst->rate_timer--;

				if (pst->rate_timer > 0) 
					continue;

				if (pst->channel_position >= 
					(int) pst->channel_ptr->channel_vec.size()) {
					pst->channel_position = 0;
					push_report = 1;
				}

				pst->rate_timer =
					pst->channel_ptr->channel_vec[pst->channel_position].dwell *
					(SERVER_TIMESLICES_SEC - pst->channel_rate);

				// printf("debug - pid %u source %s reset timer %d chdwell %d timeslices %d innaterate %d\n", getpid(), pst->strong_source->FetchInterface().c_str(), pst->rate_timer, pst->channel_ptr->channel_vec[pst->channel_position].dwell, SERVER_TIMESLICES_SEC, pst->channel_rate);

			} else if (pst->channel_dwell) {
				pst->dwell_timer--;

				if (pst->dwell_timer > 0)
					continue;

				if (pst->channel_position >= 
					(int) pst->channel_ptr->channel_vec.size()) {
					pst->channel_position = 0;
					push_report = 1;
				}

				pst->dwell_timer =
					pst->channel_ptr->channel_vec[pst->channel_position].dwell *
					(SERVER_TIMESLICES_SEC * pst->channel_dwell);
			}

			// Set and advertise the channel

			if (push_report) {

				gettimeofday(&tv, NULL);

				SubtractTimeval(&tv, &(pst->tm_hop_start), &(pst->tm_hop_time));

				pst->tm_hop_start.tv_sec = tv.tv_sec;
				pst->tm_hop_start.tv_usec = tv.tv_usec;

				// fprintf(stderr, "debug - looped channel set in %usec %uusec\n", (unsigned int) pst->tm_hop_time.tv_sec, (unsigned int) pst->tm_hop_time.tv_usec);

				SendIPCReport(pst);
			}

			// Set the local channel
			pst->channel = 
				pst->channel_ptr->channel_vec[pst->channel_position].channel;

			// printf("debug - hop list new channel %d\n", pst->channel);

			// Set the delay to be the hopdwell of the channel times the 
			// overall hopping rate
			if (pst->strong_source->SetChannel(pst->channel) < 0) {
				pst->consec_channel_err++;

				if (pst->consec_channel_err > MAX_CONSEC_CHAN_ERR) {
					_MSG("Packet source '" + pst->strong_source->FetchInterface() + 
						 "' has had too many consecutive errors and will be shut down.",
						 MSGFLAG_ERROR);
					pst->strong_source->CloseSource();
					pst->error = 1;
				}
			} else {
				pst->consec_channel_err = 0;
			}

			pst->channel_position++;
		}
	}
}

void Packetsourcetracker::ChainHandler(kis_packet *in_pack) {
	kis_datachunk *linkchunk = 
		(kis_datachunk *) in_pack->fetch(_PCM(PACK_COMP_LINKFRAME));

	if (linkchunk == NULL)
		return;

	if (running_as_ipc) {
		// Send it through the IPC system
		SendIPCPacket(in_pack, linkchunk);
	} else {
		// Send it through the packet source demangler
		kis_ref_capsource *csrc_ref = 
			(kis_ref_capsource *) in_pack->fetch(_PCM(PACK_COMP_KISCAPSRC));

		if (csrc_ref == NULL) {
			_MSG("We got a packet in the PST chainhandler with data but no capsource "
				 "reference so we don't know how to handle it, we're going to have "
				 "to throw it on the floor, something is wrong.", MSGFLAG_ERROR);
			return;
		}

		csrc_ref->ref_source->ManglePacket(in_pack, linkchunk);
	}
}

