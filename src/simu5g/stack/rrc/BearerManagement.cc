//
//                  Simu5G
//
// Authors: Andras Varga (OpenSim Ltd)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//

#include <sstream>

#include "simu5g/stack/rrc/BearerManagement.h"
#include "simu5g/stack/rrc/D2DModeController.h"
#include "simu5g/stack/rrc/Registration.h"
#include "simu5g/stack/mac/LteMacBase.h"
#include "simu5g/stack/rlc/RlcMux.h"
#include "simu5g/stack/rlc/um/UmTxEntity.h"
#include "simu5g/stack/pdcp/UpperMux.h"
#include "simu5g/stack/pdcp/DcMux.h"
#include "simu5g/stack/pdcp/PdcpTxEntityBase.h"
#include "simu5g/stack/pdcp/PdcpRxEntityBase.h"
#include "simu5g/common/InitStages.h"

namespace simu5g {

using namespace inet;

Define_Module(BearerManagement);

void BearerManagement::initialize(int stage)
{
    if (stage == inet::INITSTAGE_LOCAL) {
        registration_ = check_and_cast<Registration *>(getParentModule()->getSubmodule("registration"));

        // Resolve PDCP entity types
        pdcpTxEntityModuleType_ = cModuleType::get(par("pdcpTxEntityModuleType").stringValue());
        pdcpRxEntityModuleType_ = cModuleType::get(par("pdcpRxEntityModuleType").stringValue());
        pdcpBypassTxEntityModuleType_ = cModuleType::get(par("pdcpBypassTxEntityModuleType").stringValue());
        pdcpBypassRxEntityModuleType_ = cModuleType::get(par("pdcpBypassRxEntityModuleType").stringValue());

        // Resolve RLC entity types
        rlcUmTxEntityModuleType_ = cModuleType::get(par("rlcUmTxEntityModuleType").stringValue());
        rlcUmRxEntityModuleType_ = cModuleType::get(par("rlcUmRxEntityModuleType").stringValue());
        rlcTmTxEntityModuleType_ = cModuleType::get(par("rlcTmTxEntityModuleType").stringValue());
        rlcTmRxEntityModuleType_ = cModuleType::get(par("rlcTmRxEntityModuleType").stringValue());
        rlcAmTxEntityModuleType_ = cModuleType::get(par("rlcAmTxEntityModuleType").stringValue());
        rlcAmRxEntityModuleType_ = cModuleType::get(par("rlcAmRxEntityModuleType").stringValue());

        nicModule_ = inet::getContainingNicModule(this);

        rlcMuxModule.reference(this, "rlcMuxModule", true);
        nrRlcMuxModule.reference(this, "nrRlcMuxModule", false);
        macModule.reference(this, "macModule", true);
        nrMacModule.reference(this, "nrMacModule", false);

        // nascTime / FRER: optional second NR leg -- unresolved (empty)
        // on any NIC that doesn't have nrRlcMux2/nrMac2.
        nrRlcMuxModule2.reference(this, "nrRlcMuxModule2", false);
        nrMacModule2.reference(this, "nrMacModule2", false);

        std::string drbIdsStr = par("dcSecondaryDrbIds").stdstringValue();
        std::stringstream ss(drbIdsStr);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty())
                dcSecondaryDrbIds_.insert(std::atoi(token.c_str()));
        }
    }
}

void BearerManagement::handleMessage(cMessage *msg)
{
    throw cRuntimeError("This module does not process messages");
}

RlcLeg BearerManagement::legForDrb(int drbId, bool isNrUeSide) const
{
    if (isNrUeSide && dcSecondaryDrbIds_.count(drbId) && nrRlcMuxModule2 && nrMacModule2)
        return RlcLeg::NR_SECONDARY;
    return isNrUeSide ? RlcLeg::NR_PRIMARY : RlcLeg::LTE;
}

void BearerManagement::createIncomingConnection(FlowControlInfo *lteInfo, bool withPdcp)
{
    Enter_Method_Silent("createIncomingConnection()");

    EV << "BearerManagement::createIncomingConnection - " << " srcId=" << lteInfo->getSourceId() << " destId=" << lteInfo->getDestId()
        << " groupId=" << lteInfo->getMulticastGroupId() << " drbId=" << lteInfo->getDrbId()
        << " direction=" << dirToA(lteInfo->getDirection())
        << " withPdcp=" << (withPdcp ? "yes" : "no") << endl;

    ASSERT(lteInfo->getDestId() == registration_->getLteNodeId() || lteInfo->getDestId() == registration_->getNrNodeId() || lteInfo->getMulticastGroupId() != NODEID_NONE);

    FlowDescriptor desc = FlowDescriptor::fromFlowControlInfo(*lteInfo);
    MacNodeId senderId = desc.getSourceId();
    bool isNrUeSide = (registration_->getNodeType()==UE && isNrUe(lteInfo->getDestId())); //TODO FIXME! DOES NOT WORK FOR MULTICAST!!!!!

    // nascTime / FRER: three-way leg selection, was binary isNr.
    RlcLeg leg = legForDrb(desc.getDrbId(), isNrUeSide);

    auto mac = (leg == RlcLeg::NR_SECONDARY) ? nrMacModule2.get()
             : (leg == RlcLeg::NR_PRIMARY)   ? nrMacModule.get()
                                              : macModule.get();
    LogicalCid lcid = mac->drbIdToLcid(desc.getDrbId());
    MacCid cid = MacCid(senderId, lcid);
    mac->createIncomingConnection(cid, desc);

    // RLC entity creation
    DrbKey rlcId = ctrlInfoToRxDrbKey(lteInfo);
    auto *rlcMux = (leg == RlcLeg::NR_SECONDARY) ? nrRlcMuxModule2.get()
                 : (leg == RlcLeg::NR_PRIMARY)   ? nrRlcMuxModule.get()
                                                  : rlcMuxModule.get();
    // createAndInstallRlcRxBuffer still takes a bool isNr (unchanged) --
    // it only affects entity naming/param wiring (setRlcEntityParams uses
    // isNr to pick "^.nrMac"/"^.mac"), which is correct for NR_SECONDARY
    // too since its entities should reference nrMac2 the same way, not a
    // third naming scheme.
    bool isNrEntity = (leg != RlcLeg::LTE);
    createAndInstallRlcRxBuffer(rlcId, lteInfo, rlcMux, isNrEntity);

    // PDCP entity creation
    // nascTime / FRER: NR_SECONDARY uses pdcpMux2, a genuinely separate
    // instance -- unlike RLC, the primary NR leg shares one pdcpMux with
    // LTE natively, so this split didn't previously exist at the PDCP layer.
    auto *pdcpMux = (leg == RlcLeg::NR_SECONDARY)
        ? check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux2"))
        : check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux")); // nullptr on UEs (no X2)

    if (withPdcp) {
        DrbKey id = DrbKey(lteInfo->getSourceId(), lteInfo->getDrbId());
        std::string name = "pdcp-rx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpRxEntityModuleType_->create(name.c_str(), nicModule_);
        module->par("headerCompressedSize") = par("headerCompressedSize");
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));

        // nascTime / FRER: select the correct RLC RX registry for the leg.
        auto& rxRegistry = (leg == RlcLeg::NR_SECONDARY) ? nrRlcRxEntities2_
                          : (leg == RlcLeg::NR_PRIMARY)   ? nrRlcRxEntities_
                                                           : rlcRxEntities_;
        auto rlcIt = rxRegistry.find(rlcId);
        ASSERT(rlcIt != rxRegistry.end());
        rlcIt->second->gate("out")->connectTo(module->gate("in"));

        int fromIdx = pdcpMux->gateSize("fromRxEntity");
        pdcpMux->setGateSize("fromRxEntity", fromIdx + 1);
        module->gate("out")->connectTo(pdcpMux->gate("fromRxEntity", fromIdx));

        // DcMux/X2 wiring is native-DC-only and never applies to
        // NR_SECONDARY (which has no pdcpDcMux of its own -- it's a
        // fully independent DRB, not a split-bearer bypass leg).
        if (leg != RlcLeg::NR_SECONDARY && pdcpDcMux && module->hasGate("dcIn")) {
            int dcIdx = pdcpDcMux->gateSize("toRxEntity");
            pdcpDcMux->setGateSize("toRxEntity", dcIdx + 1);
            pdcpDcMux->gate("toRxEntity", dcIdx)->connectTo(module->gate("dcIn"));
        }

        module->scheduleStart(simTime());
        module->callInitialize();
        auto *rxEnt = check_and_cast<PdcpRxEntityBase *>(module);
        (leg == RlcLeg::NR_SECONDARY ? pdcpRxEntities2_ : pdcpRxEntities_)[id] = rxEnt;
    }
    else {
        // Native DC bypass path -- eNB-only, X2-based, unrelated to
        // NR_SECONDARY. Unchanged from stock behavior.
        ASSERT(pdcpDcMux != nullptr);
        DrbKey id = DrbKey(lteInfo->getSourceId(), lteInfo->getDrbId());
        std::string name = "pdcp-bypass-rx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpBypassRxEntityModuleType_->create(name.c_str(), nicModule_);
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));

        auto rlcIt2 = rlcRxEntities_.find(rlcId);
        ASSERT(rlcIt2 != rlcRxEntities_.end());
        rlcIt2->second->gate("out")->connectTo(module->gate("in"));

        int fromIdx = pdcpDcMux->gateSize("fromEntity");
        pdcpDcMux->setGateSize("fromEntity", fromIdx + 1);
        module->gate("out")->connectTo(pdcpDcMux->gate("fromEntity", fromIdx));

        module->scheduleStart(simTime());
        module->callInitialize();
        auto *rxEnt = check_and_cast<PdcpRxEntityBase *>(module);
        pdcpBypassRxEntities_[id] = rxEnt;
    }
}

void BearerManagement::createOutgoingConnection(FlowControlInfo *lteInfo, bool withPdcp)
{
    Enter_Method_Silent("createOutgoingConnection()");

    EV << "BearerManagement::createOutgoingConnection - " << " srcId=" << lteInfo->getSourceId() << " destId=" << lteInfo->getDestId()
        << " groupId=" << lteInfo->getMulticastGroupId() << " drbId=" << lteInfo->getDrbId()
        << " direction=" << dirToA(lteInfo->getDirection())
        << " withPdcp=" << (withPdcp ? "yes" : "no") << endl;

    ASSERT(lteInfo->getSourceId() == registration_->getLteNodeId() || lteInfo->getSourceId() == registration_->getNrNodeId());

    FlowDescriptor desc = FlowDescriptor::fromFlowControlInfo(*lteInfo);
    MacNodeId destId = desc.getDestId();
    bool isNrUeSide = (registration_->getNodeType()==UE && isNrUe(lteInfo->getSourceId()));

    RlcLeg leg = legForDrb(desc.getDrbId(), isNrUeSide);

    auto mac = (leg == RlcLeg::NR_SECONDARY) ? nrMacModule2.get()
             : (leg == RlcLeg::NR_PRIMARY)   ? nrMacModule.get()
                                              : macModule.get();
    LogicalCid lcid = mac->drbIdToLcid(desc.getDrbId());
    MacCid cid = MacCid(destId, lcid);
    mac->createOutgoingConnection(cid, desc);

    DrbKey rlcId = ctrlInfoToTxDrbKey(lteInfo);
    auto *rlcMux = (leg == RlcLeg::NR_SECONDARY) ? nrRlcMuxModule2.get()
                 : (leg == RlcLeg::NR_PRIMARY)   ? nrRlcMuxModule.get()
                                                  : rlcMuxModule.get();
    bool isNrEntity = (leg != RlcLeg::LTE);
    createAndInstallRlcTxBuffer(rlcId, lteInfo, rlcMux, isNrEntity);

    auto *pdcpMux = (leg == RlcLeg::NR_SECONDARY)
        ? check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux2"))
        : check_and_cast<UpperMux *>(nicModule_->getSubmodule("pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux")); // nullptr on UEs (no X2)

    if (withPdcp) {
        DrbKey id = DrbKey(lteInfo->getDestId(), lteInfo->getDrbId());

        // Native EN-DC master-wiring shortcut (nrOut gate) is skipped
        // entirely for NR_SECONDARY -- that shortcut only applies to the
        // LTE-anchor/NR-secondary EN-DC pairing, not a genuine second NR
        // leg, which always gets its own independent PDCP entity.
        bool wiredToMaster = false;
        if (leg != RlcLeg::NR_SECONDARY && registration_->getNodeType()==UE && isNrUe(lteInfo->getSourceId())) {
            for (auto& [key, masterEntity] : pdcpTxEntities_) {
                if (key.getDrbId() == id.getDrbId()) {
                    auto *masterModule = check_and_cast<cModule *>(masterEntity);
                    if (masterModule->hasGate("nrOut")) {
                        auto nrRlcIt = nrRlcTxEntities_.find(rlcId);
                        ASSERT(nrRlcIt != nrRlcTxEntities_.end());
                        masterModule->gate("nrOut")->connectTo(nrRlcIt->second->gate("in"));
                        wiredToMaster = true;
                    }
                    break;
                }
            }
        }
        if (!wiredToMaster) {
            std::string name = "pdcp-tx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
            auto *module = pdcpTxEntityModuleType_->create(name.c_str(), nicModule_);
            module->par("headerCompressedSize") = par("headerCompressedSize");
            module->finalizeParameters();
            module->buildInside();
            setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));

            int idx = pdcpMux->gateSize("toTxEntity");
            pdcpMux->setGateSize("toTxEntity", idx + 1);
            pdcpMux->gate("toTxEntity", idx)->connectTo(module->gate("in"));

            auto& txRegistry = (leg == RlcLeg::NR_SECONDARY) ? nrRlcTxEntities2_
                              : (leg == RlcLeg::NR_PRIMARY)   ? nrRlcTxEntities_
                                                               : rlcTxEntities_;
            auto rlcIt = txRegistry.find(rlcId);
            ASSERT(rlcIt != txRegistry.end());
            module->gate("out")->connectTo(rlcIt->second->gate("in"));

            if (leg != RlcLeg::NR_SECONDARY && pdcpDcMux && module->hasGate("dcOut")) {
                int dcIdx = pdcpDcMux->gateSize("fromEntity");
                pdcpDcMux->setGateSize("fromEntity", dcIdx + 1);
                module->gate("dcOut")->connectTo(pdcpDcMux->gate("fromEntity", dcIdx));
            }

            module->scheduleStart(simTime());
            module->callInitialize();
            auto *txEnt = check_and_cast<PdcpTxEntityBase *>(module);
            pdcpMux->registerTxEntity(id, txEnt);
            (leg == RlcLeg::NR_SECONDARY ? pdcpTxEntities2_ : pdcpTxEntities_)[id] = txEnt;
        }
    }
    else {
        // Native DC bypass path -- eNB-only, X2-based, unchanged.
        ASSERT(pdcpDcMux != nullptr);
        DrbKey id = DrbKey(lteInfo->getDestId(), lteInfo->getDrbId());
        std::string name = "pdcp-bypass-tx-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
        auto *module = pdcpBypassTxEntityModuleType_->create(name.c_str(), nicModule_);
        module->finalizeParameters();
        module->buildInside();
        setEntityDisplayPosition(module, true, rlcMux, num(id.getDrbId()));

        int idx = pdcpDcMux->gateSize("toBypassTxEntity");
        pdcpDcMux->setGateSize("toBypassTxEntity", idx + 1);
        pdcpDcMux->gate("toBypassTxEntity", idx)->connectTo(module->gate("in"));

        auto rlcIt2 = rlcTxEntities_.find(rlcId);
        ASSERT(rlcIt2 != rlcTxEntities_.end());
        module->gate("out")->connectTo(rlcIt2->second->gate("in"));

        module->scheduleStart(simTime());
        module->callInitialize();
        auto *txEnt = check_and_cast<PdcpTxEntityBase *>(module);
        pdcpDcMux->registerBypassTxEntity(id, txEnt);
        pdcpBypassTxEntities_[id] = txEnt;
    }
}

void BearerManagement::setRlcEntityParams(cModule *entity, bool isNr)
{
    if (entity->hasPar("macModule"))
        entity->par("macModule").setStringValue(isNr ? "^.nrMac" : "^.mac");
    if (entity->hasPar("isNR"))
        entity->par("isNR").setBoolValue(isNr);
}

void BearerManagement::setEntityDisplayPosition(cModule *entity, bool isPdcpEntity, cModule *rlcMux, int bearerIndex)
{
    auto *pdcpMux = nicModule_->getSubmodule("pdcpMux");
    if (!pdcpMux || !rlcMux)
        return;

    int uy = atoi(pdcpMux->getDisplayString().getTagArg("p", 1));
    int lx = atoi(rlcMux->getDisplayString().getTagArg("p", 0));
    int ly = atoi(rlcMux->getDisplayString().getTagArg("p", 1));

    int x = lx + 60 * bearerIndex;
    int y = isPdcpEntity ? uy + (ly - uy) / 3 : uy + 2 * (ly - uy) / 3;

    entity->getDisplayString().setTagArg("p", 0, x);
    entity->getDisplayString().setTagArg("p", 1, y);
}

RlcTxEntityBase *BearerManagement::createAndInstallRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr)
{
    LteRlcType rlcType = static_cast<LteRlcType>(lteInfo->getRlcType());
    cModuleType *moduleType;
    const char *prefix;
    switch (rlcType) {
        case TM: moduleType = rlcTmTxEntityModuleType_; prefix = "tm-tx"; break;
        case AM: moduleType = rlcAmTxEntityModuleType_; prefix = "am-tx"; break;
        default: moduleType = rlcUmTxEntityModuleType_; prefix = "um-tx"; break;
    }
    std::string name = std::string(isNr ? "nrRlc-" : "rlc-") + prefix + "-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
    auto *module = moduleType->create(name.c_str(), nicModule_);
    setRlcEntityParams(module, isNr);
    module->finalizeParameters();
    module->buildInside();
    setEntityDisplayPosition(module, false, rlcMux, num(id.getDrbId()));

    int fromIdx = rlcMux->gateSize("fromTxEntity");
    rlcMux->setGateSize("fromTxEntity", fromIdx + 1);
    module->gate("out")->connectTo(rlcMux->gate("fromTxEntity", fromIdx));

    int macIdx = rlcMux->gateSize("macToTxEntity");
    rlcMux->setGateSize("macToTxEntity", macIdx + 1);
    rlcMux->gate("macToTxEntity", macIdx)->connectTo(module->gate("macIn"));

    module->scheduleStart(simTime());
    module->callInitialize();

    RlcTxEntityBase *txEnt = check_and_cast<RlcTxEntityBase *>(module);
    txEnt->setFlowControlInfo(lteInfo);

    // nascTime / FRER: registry selection now needs to distinguish
    // NR_SECONDARY from NR_PRIMARY, not just isNr. Since this helper only
    // receives a bool, the caller (createOutgoingConnection) picks the
    // right registry itself after this call returns for TX; this function
    // still writes to the legacy two registries by rlcMux identity, which
    // is safe because rlcMux is already leg-specific by the time it's
    // passed in -- but the registry write below needs the SAME leg
    // information the caller already resolved. Simplest correct fix:
    // compare rlcMux against the known secondary mux pointer directly.
    if (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get())
        nrRlcTxEntities2_[id] = txEnt;
    else
        (isNr ? nrRlcTxEntities_ : rlcTxEntities_)[id] = txEnt;

    if (rlcType == UM) {
        auto *d2dCtrl = dynamic_cast<D2DModeController *>(nicModule_->getSubmodule("rrc")->getSubmodule("d2dModeController"));
        if (d2dCtrl) {
            auto *umTxEnt = check_and_cast<UmTxEntity *>(txEnt);
            d2dCtrl->registerD2DPeerTxEntity(MacNodeId(lteInfo->getD2dRxPeerId()), umTxEnt);
        }
    }

    return txEnt;
}

RlcRxEntityBase *BearerManagement::createAndInstallRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo, RlcMux *rlcMux, bool isNr)
{
    LteRlcType rlcType = static_cast<LteRlcType>(lteInfo->getRlcType());
    cModuleType *moduleType;
    const char *prefix;
    switch (rlcType) {
        case TM: moduleType = rlcTmRxEntityModuleType_; prefix = "tm-rx"; break;
        case AM: moduleType = rlcAmRxEntityModuleType_; prefix = "am-rx"; break;
        default: moduleType = rlcUmRxEntityModuleType_; prefix = "um-rx"; break;
    }
    std::string name = std::string(isNr ? "nrRlc-" : "rlc-") + prefix + "-" + std::to_string(num(id.getNodeId())) + "-" + std::to_string(num(id.getDrbId()));
    auto *module = moduleType->create(name.c_str(), nicModule_);
    setRlcEntityParams(module, isNr);
    module->finalizeParameters();
    module->buildInside();
    setEntityDisplayPosition(module, false, rlcMux, num(id.getDrbId()));

    int idx = rlcMux->gateSize("toRxEntity");
    rlcMux->setGateSize("toRxEntity", idx + 1);
    rlcMux->gate("toRxEntity", idx)->connectTo(module->gate("in"));

    module->scheduleStart(simTime());
    module->callInitialize();

    RlcRxEntityBase *rxEnt = check_and_cast<RlcRxEntityBase *>(module);
    rxEnt->setFlowControlInfo(lteInfo);

    rlcMux->registerRxBuffer(id, rxEnt);

    // nascTime / FRER: same registry-selection note as createAndInstallRlcTxBuffer.
    if (isNr && nrRlcMuxModule2 && rlcMux == nrRlcMuxModule2.get())
        nrRlcRxEntities2_[id] = rxEnt;
    else
        (isNr ? nrRlcRxEntities_ : rlcRxEntities_)[id] = rxEnt;

    return rxEnt;
}

RlcTxEntityBase *BearerManagement::createRlcTxBuffer(DrbKey id, FlowControlInfo *lteInfo)
{
    Enter_Method_Silent("createRlcTxBuffer()");
    return createAndInstallRlcTxBuffer(id, lteInfo, rlcMuxModule.get(), false);
}

RlcRxEntityBase *BearerManagement::createRlcRxBuffer(DrbKey id, FlowControlInfo *lteInfo)
{
    Enter_Method_Silent("createRlcRxBuffer()");
    return createAndInstallRlcRxBuffer(id, lteInfo, rlcMuxModule.get(), false);
}

void BearerManagement::deleteLocalPdcpEntities(MacNodeId nodeId, RlcLeg leg)
{
    Enter_Method_Silent("deleteLocalPdcpEntities()");

    bool isEnb = (registration_->getNodeType() == NODEB);

    // nascTime / FRER FIX: previously "isEnb ? filter-by-nodeId : true" --
    // the UE branch deleted ALL entities unconditionally, regardless of
    // nodeId. That's harmless for a UE with exactly one gNB per leg (the
    // old assumption) but would silently wipe out an unrelated,
    // still-attached leg's entities the first time ANY leg independently
    // handed over or detached. Now always filters by nodeId, for both
    // node types.
    auto& txMap = (leg == RlcLeg::NR_SECONDARY) ? pdcpTxEntities2_ : pdcpTxEntities_;
    auto& rxMap = (leg == RlcLeg::NR_SECONDARY) ? pdcpRxEntities2_ : pdcpRxEntities_;

    auto *pdcpMux = check_and_cast<UpperMux *>(
        nicModule_->getSubmodule(leg == RlcLeg::NR_SECONDARY ? "pdcpMux2" : "pdcpMux"));
    auto *pdcpDcMux = dynamic_cast<DcMux *>(nicModule_->getSubmodule("pdcpDcMux"));

    for (auto it = txMap.begin(); it != txMap.end(); ) {
        if (it->first.getNodeId() == nodeId) {
            pdcpMux->unregisterTxEntity(it->first);
            it->second->deleteModule();
            it = txMap.erase(it);
        } else ++it;
    }

    for (auto it = rxMap.begin(); it != rxMap.end(); ) {
        if (it->first.getNodeId() == nodeId) {
            it->second->deleteModule();
            it = rxMap.erase(it);
        } else ++it;
    }

    // Bypass entities are native-DC/X2-only, never used by NR_SECONDARY.
    if (leg != RlcLeg::NR_SECONDARY) {
        ASSERT(pdcpBypassTxEntities_.empty() || pdcpDcMux != nullptr);
        for (auto it = pdcpBypassTxEntities_.begin(); it != pdcpBypassTxEntities_.end(); ) {
            if (it->first.getNodeId() == nodeId) {
                pdcpDcMux->unregisterBypassTxEntity(it->first);
                it->second->deleteModule();
                it = pdcpBypassTxEntities_.erase(it);
            } else ++it;
        }
        for (auto it = pdcpBypassRxEntities_.begin(); it != pdcpBypassRxEntities_.end(); ) {
            if (it->first.getNodeId() == nodeId) {
                it->second->deleteModule();
                it = pdcpBypassRxEntities_.erase(it);
            } else ++it;
        }
    }
}

void BearerManagement::deleteLocalRlcQueues(MacNodeId nodeId, RlcLeg leg)
{
    Enter_Method_Silent("deleteLocalRlcQueues()");

    bool isEnb = (registration_->getNodeType() == NODEB);

    // At a NODEB, entities are always stored in the LTE-named maps
    // regardless of the caller's leg: createIncoming/OutgoingConnection()
    // computes isNr as (nodeType==UE && ...), always false at a gNB, and
    // gnb2 (per the confirmed design) has no second NR leg of its own --
    // it's an ordinary single-NR-stack gNB from its own perspective.
    RlcLeg effectiveLeg = isEnb ? RlcLeg::LTE : leg;

    auto& txMap = (effectiveLeg == RlcLeg::NR_SECONDARY) ? nrRlcTxEntities2_
                : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? nrRlcTxEntities_
                                                          : rlcTxEntities_;
    auto& rxMap = (effectiveLeg == RlcLeg::NR_SECONDARY) ? nrRlcRxEntities2_
                : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? nrRlcRxEntities_
                                                          : rlcRxEntities_;
    RlcMux *rlcMux = (effectiveLeg == RlcLeg::NR_SECONDARY) ? (nrRlcMuxModule2 ? nrRlcMuxModule2.get() : nullptr)
                    : (effectiveLeg == RlcLeg::NR_PRIMARY)   ? (nrRlcMuxModule ? nrRlcMuxModule.get() : nullptr)
                                                              : rlcMuxModule.get();
    if (!rlcMux)
        return;

    for (auto it = txMap.begin(); it != txMap.end(); ) {
        if (isEnb ? it->first.getNodeId() == nodeId : true) {
            it->second->deleteModule();
            it = txMap.erase(it);
        } else ++it;
    }

    for (auto it = rxMap.begin(); it != rxMap.end(); ) {
        if (isEnb ? it->first.getNodeId() == nodeId : true) {
            rlcMux->unregisterRxBuffer(it->first);
            it->second->deleteModule();
            it = rxMap.erase(it);
        } else ++it;
    }
}

PdcpRxEntityBase *BearerManagement::lookupPdcpRxEntity(DrbKey id)
{
    auto it = pdcpRxEntities_.find(id);
    if (it != pdcpRxEntities_.end())
        return it->second;
    auto it2 = pdcpRxEntities2_.find(id);
    if (it2 != pdcpRxEntities2_.end())
        return it2->second;
    auto it3 = pdcpBypassRxEntities_.find(id);
    return it3 != pdcpBypassRxEntities_.end() ? it3->second : nullptr;
}

RlcTxEntityBase *BearerManagement::lookupRlcTxBuffer(DrbKey id)
{
    auto it = rlcTxEntities_.find(id);
    if (it != rlcTxEntities_.end())
        return it->second;
    auto it2 = nrRlcTxEntities_.find(id);
    if (it2 != nrRlcTxEntities_.end())
        return it2->second;
    auto it3 = nrRlcTxEntities2_.find(id);
    return it3 != nrRlcTxEntities2_.end() ? it3->second : nullptr;
}

PdcpTxEntityBase *BearerManagement::lookupPdcpTxEntity(DrbKey id)
{
    auto it = pdcpTxEntities_.find(id);
    if (it != pdcpTxEntities_.end())
        return it->second;
    auto it2 = pdcpTxEntities2_.find(id);
    return it2 != pdcpTxEntities2_.end() ? it2->second : nullptr;
}

void BearerManagement::pdcpActiveUeUL(std::set<MacNodeId> *ueSet)
{
    for (const auto& [id, rxEntity] : pdcpRxEntities_) {
        if (!rxEntity->isEmpty())
            ueSet->insert(id.getNodeId());
    }
    for (const auto& [id, rxEntity] : pdcpRxEntities2_) {
        if (!rxEntity->isEmpty())
            ueSet->insert(id.getNodeId());
    }
}

} // namespace simu5g
