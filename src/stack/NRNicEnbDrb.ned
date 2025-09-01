//
//                  Simu5G
//
// Authors: Mohamed Seliem (University College Cork)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//


package simu5g.stack;

import simu5g.stack.sdap.NrRxSdapEntity;
import simu5g.stack.pdcp_rrc.NRPdcpRrcEnb;
import simu5g.stack.rlc.LteRlc;
import simu5g.stack.mac.ILteMac;
import simu5g.stack.phy.ILtePhy;
import simu5g.stack.multiplexer.RlcToMacMultiplexer;
//
// This module extends the Enb NIC to insert DRB functionality 
// allowing QoS Flow management per 3GPP 5G specifications.
//
module NRNicEnbDrb extends LteNicEnbD2D
{
    parameters:
        nrSdapDisabled = false;
        drbDisabled = false;
        
        int numDrbs; // this to allow multiple DRBs
        @display("bgb=694,443");
        LteChannelModelType = default("NRChannelModel_3GPP38_901");

        packetFlowManager.typename = default("NRPacketFlowManagerGnb");


    submodules:

        nrRxSdapEntity: NrRxSdapEntity {
            numDrbs = parent.numDrbs;
            @display("p=245,67");
        }

        // CASE SDAP active: → multiple DRBs activated
        nrPdcpRrc[numDrbs]: NRPdcpRrcEnb if !nrSdapDisabled {
            @display("p=245,149");
            drbIndex = index;  // assign DRB ID
        }

		// RLC Layer
        nrRlc[numDrbs]: LteRlc {
            @display("p=245,231");
            drbIndex = index;
            d2dCapable = parent.d2dCapable;
            *.macModule = "^.^.mac";
            *.packetFlowManagerModule = parent.hasRNISupport ? "^.^.packetFlowManager" : "";
            um.nodeType = parent.nodeType;

        }

        rlcToMacMultiplexer: RlcToMacMultiplexer {
            numDrbs = parent.numDrbs;
            @display("p=245,314");
        }
    connections:
        ip2nic.stackNic <--> nrRxSdapEntity.DataPort;


		// New multi-DRB connections when SDAP is enabled
        for i=0..numDrbs-1 {
            nrRxSdapEntity.stackSdap[i] <--> nrPdcpRrc[i].DataPort;

            //# PDCP <-> RLC
            nrPdcpRrc[i].AM_Sap++ <--> nrRlc[i].AM_Sap;
            nrPdcpRrc[i].UM_Sap++ <--> nrRlc[i].UM_Sap;
            nrPdcpRrc[i].TM_Sap++ <--> nrRlc[i].TM_Sap;

            //# RLC <-> MUX
            nrRlc[i].RLC_to_MAC --> rlcToMacMultiplexer.rlcIn[i];
            nrRlc[i].MAC_to_RLC <-- rlcToMacMultiplexer.rlcOut[i];
        }

        rlcToMacMultiplexer.macOut --> mac.RLC_to_MAC;
        rlcToMacMultiplexer.macIn <-- mac.MAC_to_RLC;

}

