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

// Panel has to be here to pass configure, so just test these
#if (defined(HAVE_LIBNCURSES) || defined (HAVE_LIBCURSES))

#include "util.h"
#include "messagebus.h"
#include "kis_panel_widgets.h"
#include "kis_panel_windows.h"
#include "kis_panel_frontend.h"

// STATUS protocol parser that injects right into the messagebus
void KisPanelClient_STATUS(CLIPROTO_CB_PARMS) {
	if (proto_parsed->size() < 2) {
		return;
	}

	int flags;
	string text;

	text = (*proto_parsed)[0].word;

	if (sscanf((*proto_parsed)[1].word.c_str(), "%d", &flags) != 1) {
		return;
	}

	_MSG(text, flags);
}

void KisPanelClient_CARD(CLIPROTO_CB_PARMS) {
	// Pass it off to the clinet frame
	((KisPanelInterface *) auxptr)->NetClientCARD(globalreg, proto_string,
												  proto_parsed, srccli, auxptr);
}

void KisPanelInterface::NetClientCARD(CLIPROTO_CB_PARMS) {
	// interface		0
	// type				1
	// username			2
	// channel			3
	// uuid				4
	// packets			5
	// hopping			6
	// MAX				7
	
	if (proto_parsed->size() < 7) {
		return;
	}

	// Grab the UUID first, see if we need to build a record or if we're
	// filling in an existing one
	uuid carduuid = uuid((*proto_parsed)[4].word);
	if (carduuid.error) {
		_MSG("Invalid UUID in CARD protocol, skipping line", MSGFLAG_ERROR);
		return;
	}

	KisPanelInterface::knc_card *card = NULL;
	int prevknown = 0;
	map<uuid, KisPanelInterface::knc_card *>::iterator itr;

	if ((itr = netcard_map.find(carduuid)) != netcard_map.end()) {
		card = itr->second;
		prevknown = 1;
	} else {
		card = new KisPanelInterface::knc_card;
	}

	// If we didn't know about it, get the name, type, etc.  Otherwise
	// we can ignore it because we should never change this
	if (prevknown == 0) {
		card->carduuid = carduuid;
		card->uuid_hash = Adler32Checksum(carduuid.UUID2String().c_str(),
										  carduuid.UUID2String().length());
		card->interface = MungeToPrintable((*proto_parsed)[0].word);
		card->type = MungeToPrintable((*proto_parsed)[1].word);
		card->username = MungeToPrintable((*proto_parsed)[2].word);
	}

	// Parse the current channel and number of packets for all of them
	int tchannel;
	int tpackets;
	int thopping;

	if (sscanf((*proto_parsed)[3].word.c_str(), "%d", &tchannel) != 1) {
		_MSG("Invalid channel in CARD protocol, skipping line.", MSGFLAG_ERROR);
		if (prevknown == 0)
			delete card;
		return;
	}

	if (sscanf((*proto_parsed)[5].word.c_str(), "%d", &tpackets) != 1) {
		_MSG("Invalid packet count in CARD protocol, skipping line.", MSGFLAG_ERROR);
		if (prevknown == 0)
			delete card;
		return;
	}

	if (sscanf((*proto_parsed)[6].word.c_str(), "%d", &thopping) != 1) {
		_MSG("Invalid hop state in CARD protocol, skipping line.", MSGFLAG_ERROR);
		if (prevknown == 0)
			delete card;
		return;
	}

	// We're good, lets fill it in
	card->channel = tchannel;
	card->packets = tpackets;
	card->hopping = thopping;

	// Fill in the last time we saw something here
	card->last_update = time(0);

	if (prevknown == 0) 
		netcard_map[carduuid] = card;

}

void KisPanelClient_Configured(CLICONF_CB_PARMS) {
	((KisPanelInterface *) auxptr)->NetClientConfigure(kcli, recon);
}

KisPanelInterface::KisPanelInterface() {
	fprintf(stderr, "FATAL OOPS: KisPanelInterface not called with globalreg\n");
	exit(-1);
}

KisPanelInterface::KisPanelInterface(GlobalRegistry *in_globalreg) :
	PanelInterface(in_globalreg) {
	globalreg = in_globalreg;

}

KisPanelInterface::~KisPanelInterface() {
	for (unsigned int x = 0; x < netclient_vec.size(); x++)
		delete netclient_vec[x];
}

int KisPanelInterface::AddNetClient(string in_host, int in_reconnect) {
	KisNetClient *netcl = new KisNetClient(globalreg);

	netcl->AddConfCallback(KisPanelClient_Configured, 1, this);

	if (netcl->Connect(in_host, in_reconnect) < 0)
		return -1;

	netclient_vec.push_back(netcl);

	return 1;
}

vector<KisNetClient *> KisPanelInterface::FetchNetClientVec() {
	return netclient_vec;
}

int KisPanelInterface::RemoveNetClient(KisNetClient *in_cli) {
	for (unsigned int x = 0; x < netclient_vec.size(); x++) {
		if (netclient_vec[x] == in_cli) {
			delete netclient_vec[x];
			netclient_vec.erase(netclient_vec.begin() + x);
			return 1;
		}
	}

	return 0;
}

void KisPanelInterface::NetClientConfigure(KisNetClient *in_cli, int in_recon) {
	if (in_recon)
		return;

	_MSG("Got configure event for client", MSGFLAG_INFO);

	if (in_cli->RegisterProtoHandler("STATUS", "text,flags",
									 KisPanelClient_STATUS, this) < 0) {
		_MSG("Could not register STATUS protocol with remote server, connection "
			 "will be terminated.", MSGFLAG_ERROR);
		in_cli->KillConnection();
	}
	if (in_cli->RegisterProtoHandler("CARD", 
									 "interface,type,username,channel,"
									 "uuid,packets,hopping",
									 KisPanelClient_CARD, this) < 0) {
		_MSG("Could not register CARD protocol with remote server, connection "
			 "will be terminated.", MSGFLAG_ERROR);
		in_cli->KillConnection();
	}
}

void KisPanelInterface::RaiseAlert(string in_title, string in_text) {
	Kis_ModalAlert_Panel *ma = new Kis_ModalAlert_Panel(globalreg, this);

	ma->Position((LINES / 2) - 5, (COLS / 2) - 20, 10, 40);

	ma->ConfigureAlert(in_title, in_text);
	
	globalreg->panel_interface->AddPanel(ma);

}

map<uuid, KisPanelInterface::knc_card *> *KisPanelInterface::FetchNetCardMap() {
	return &netcard_map;
}

#endif

