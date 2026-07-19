//
//                  Simu5G
//
// Authors: Giovanni Nardini, Giovanni Stea, Antonio Virdis (University of Pisa)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//

#include "HandoverPacketHolderUe.h"

#include <inet/linklayer/common/InterfaceTag_m.h>
#include <inet/common/socket/SocketTag_m.h>
#include "simu5g/common/binder/Binder.h"
#include "simu5g/common/LteControlInfoTags_m.h"
namespace simu5g {
using namespace inet;
using namespace omnetpp;
Define_Module(HandoverPacketHolderUe);

HandoverPacketHolderUe::~HandoverPacketHolderUe()
{
    while (!ueHoldFromIp_.empty()) {
        Packet *pkt = ueHoldFromIp_.front();
        ueHoldFromIp_.pop_front();
        delete pkt;
    }
    // nascTime / FRER: mirror cleanup for the DC-secondary queue.
    while (!ueHoldFromIp2_.empty()) {
        Packet *pkt = ueHoldFromIp2_.front();
        ueHoldFromIp2_.pop_front();
        delete pkt;
    }
}

void HandoverPacketHolderUe::initialize(int stage)
{
    if (stage == inet::INITSTAGE_LOCAL) {
        stackGateOut_ = gate("stackOut");
        binder_.reference(this, "binderModule", true);
        cModule *ue = getContainingNode(this);
        nodeId_ = MacNodeId(ue->par("macNodeId").intValue());
        if (ue->hasPar("nrMacNodeId"))
            nrNodeId_ = MacNodeId(ue->par("nrMacNodeId").intValue());
    }
    else if (stage == INITSTAGE_SIMU5G_BINDER_ACCESS) {
        // get serving node IDs -- note the this is STLL not late enough to pick up the result of dynamic cell association
        if (nodeId_ != NODEID_NONE)
            servingNodeId_ = binder_->getServingNode(nodeId_);
        if (nrNodeId_ != NODEID_NONE)
            nrServingNodeId_ = binder_->getServingNode(nrNodeId_);
        // nascTime / FRER: no equivalent static initial read for
        // nrServingNodeId2_ -- the DC-secondary leg has no static attach
        // path at all (confirmed via Registration.cc), it only ever
        // attaches dynamically, so this starts at NODEID_NONE and is only
        // ever set via triggerHandoverUe(isDcSecondary=true).
    }
}

void HandoverPacketHolderUe::handleMessage(cMessage *msg)
{
    if (!msg->getArrivalGate()->isName("upperLayerIn"))
        throw cRuntimeError("Message received on wrong gate %s", msg->getArrivalGate()->getFullName());
    auto pkt = check_and_cast<Packet *>(msg);
    fromIpUe(pkt);
}

void HandoverPacketHolderUe::fromIpUe(Packet *datagram)
{
    EV << "HandoverPacketHolder::fromIpUe - message from IP layer: send to stack: " << datagram->str() << std::endl;
    datagram->removeTagIfPresent<SocketInd>();
    removeAllSimu5GTags(datagram);
    datagram->removeTagIfPresent<InterfaceReq>();

    // nascTime / FRER: NOTE -- this function has no way to know, from the
    // packet alone, whether it belongs to the primary or DC-secondary NR
    // leg (that information lives in FlowControlInfo/DRB routing, already
    // stripped by removeAllSimu5GTags() above by the time we'd check it).
    // Left as ueHold_/ueHoldFromIp_ (primary-leg gating) for now -- this
    // means a DC-secondary leg's own hold-during-handover state
    // (ueHold2_/ueHoldFromIp2_) is tracked correctly by
    // triggerHandoverUe()/signalHandoverCompleteUe() below, but nothing in
    // THIS function currently branches traffic into the "2" queue during a
    // hold. Flagging as a genuine open gap, not silently fixed -- routing
    // packets to the correct hold queue would need the DRB/leg identified
    // before the tag strip, which is a larger change than this pass covers.
    if (ueHold_) {
        ueHoldFromIp_.push_back(datagram);
    }
    else {
        if (servingNodeId_ == NODEID_NONE && nrServingNodeId_ == NODEID_NONE) { // UE is detached
            EV << "HandoverPacketHolder::fromIpUe - UE is not attached to any serving node. Delete packet." << endl;
            delete datagram;
        }
        else
            toStackUe(datagram);
    }
}

void HandoverPacketHolderUe::toStackUe(Packet *pkt)
{
    send(pkt, stackGateOut_);
}

void HandoverPacketHolderUe::triggerHandoverUe(MacNodeId newMasterId, bool isNr, bool isDcSecondary)
{
    EV << NOW << " HandoverPacketHolder::triggerHandoverUe - start holding packets" << endl;
    // nascTime / FRER: three-way selection, was two-way. isDcSecondary
    // only ever meaningful when isNr is also true (mirrors HandoverController's
    // own isDcSecondary_ semantics -- an LTE leg is never DC-secondary).
    if (newMasterId != NODEID_NONE) {
        if (!isNr) {
            ueHold_ = true;
            servingNodeId_ = newMasterId;
        }
        else if (isDcSecondary) {
            ueHold2_ = true;
            nrServingNodeId2_ = newMasterId;
        }
        else {
            ueHold_ = true;
            nrServingNodeId_ = newMasterId;
        }
    }
    else {
        if (!isNr)
            servingNodeId_ = NODEID_NONE;
        else if (isDcSecondary)
            nrServingNodeId2_ = NODEID_NONE;
        else
            nrServingNodeId_ = NODEID_NONE;
    }
}

void HandoverPacketHolderUe::signalHandoverCompleteUe(bool isNr, bool isDcSecondary)
{
    Enter_Method("signalHandoverCompleteUe");
    // nascTime / FRER: three-way selection, was two-way.
    MacNodeId servingNodeId = !isNr ? servingNodeId_ : (isDcSecondary ? nrServingNodeId2_ : nrServingNodeId_);
    if (servingNodeId != NODEID_NONE) {
        if (isDcSecondary) {
            while (!ueHoldFromIp2_.empty()) {
                auto pkt = ueHoldFromIp2_.front();
                ueHoldFromIp2_.pop_front();
                toStackUe(pkt);
            }
            ueHold2_ = false;
        }
        else {
            while (!ueHoldFromIp_.empty()) {
                auto pkt = ueHoldFromIp_.front();
                ueHoldFromIp_.pop_front();
                toStackUe(pkt);
            }
            ueHold_ = false;
        }
    }
}
} //namespace
