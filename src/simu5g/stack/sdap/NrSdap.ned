//
//                  Simu5G
//
// Authors: Mohamed Seliem (University College Cork), Afshin Zanganeh and How-Hang Liu
//          (Technical University of Dresden)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//

package simu5g.stack.sdap;

//
// Implements the SDAP Protocol.
//
// For PDUs received from the PDCP layer, extracts SDAP headers (if present),
// restores the associated QoS Flow based on QFI, and forwards the resulting SDUs
// to the upper layer (IP, Ethernet, or application for unstructured sessions).
//
// For SDUs received from the upper layer, performs QFI-to-DRB mapping,
// encapsulates the data with SDAP headers if needed, and forwards the packets
// to the PDCP layer. On the gNB DL path, the QFI is always taken from the
// QfiReq tag (set by GtpUser). On the UE UL path, the QFI resolution chain is:
//   1. QfiReq tag (if set by the application)
//   2. Reflective QoS table (if enabled)
//   3. DSCP field of the IP header (if useDscpAsQfiFallback is enabled)
//   4. QFI=0 (falls back to default DRB)
//
// An SDAP header is added when the QFI cannot be recovered from the DRB alone,
// or when reflective QoS needs to carry the RQI bit on a dedicated DRB.
//
// Supports all 3GPP PDU session types (IPv4, IPv6, Ethernet, Unstructured)
// via the pduSessionType field in drbConfig.
//
// drbConfig fields:
//  - drb (int): DRB identifier (= nrPdcp[]/nrRlc[] array index)
//  - ue (int, optional): UE MacNodeId (gNB only; omit on UE side)
//  - qfiList (int[]): QFI values mapped to this DRB
//  - rlcType (string, optional): RLC mode ("AM", "UM", "TM"); default: "UM"
//  - trafficClass (string, optional): UE uplink logical-channel priority;
//    default: "BACKGROUND"
//  - pduSessionType (string, optional): "IPv4", "IPv6", "Ethernet", "Unstructured";
//    default: "IPv4"
//  - upperProtocol (string, optional): INET protocol name for upper layer dispatch
//    (this is normally only needed for "Unstructured" PDU sessions); default:
//    derived from pduSessionType
//  - isDefault (bool, optional): marks this as the default DRB for the UE;
//    if omitted, the first DRB entry per UE (per nodeId) is automatically used
//    as default. The default DRB is the fallback when no QFI-to-DRB mapping matches.
//
// Gates:
//  - upperLayerIn/upperLayerOut: connects to the upper layer (Ip2Nic or app).
//  - pdcpIn/pdcpOut: connects to the PDCP layer.
//
simple NrSdap like INrSdap
{
    parameters:
        string nodeRole @enum("UE", "gNB");  // Node role: "UE" or "gNB" - determines reflective QoS behavior
        string binderModule = default("binder");
        string pdcpMuxModule = default("^.pdcpMux");
        object drbConfig = default([]);  // JSON array: [{drb, ue, qfiList, rlcType, trafficClass, pduSessionType, upperProtocol, isDefault}, ...]; trafficClass also selects the UE uplink LCG
        string reflectiveQosTableModule = default("");  // UE only: path to ReflectiveQosTable module (empty = disabled)
        bool useReflectiveQos = default(false); // gNB marks DL packets; UE expects SDAP headers for marked flows
        bool useDscpAsQfiFallback = default(false);  // UE only: use IP DSCP value as QFI when no QfiReq or reflective QoS match
        @display("i=block/layer");
    gates:
        input upperLayerIn;     // from upper layer (Ip2Nic or app)
        output upperLayerOut;   // to upper layer (Ip2Nic or app)
        input pdcpIn;           // from PDCP layer
        output pdcpOut;         // to PDCP layer
}
