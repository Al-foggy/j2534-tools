#include "iso15765.h"
#include <assert.h>
#include <chrono>
#include <thread>
#include <algorithm>

#include <string.h>

#include "utils.h"

#define LOG_DEBUG(...) printf(__VA_ARGS__); printf("\n"); fflush(stdout)

LibraryISO15765::LibraryISO15765(const LibraryPtr &library) : mLibrary(library) {

}

LibraryISO15765::~LibraryISO15765() {
}

DevicePtr LibraryISO15765::open(void *pName) {
    DevicePtr ret = mLibrary->open(pName);
	ret = std::make_shared<DeviceISO15765>(std::static_pointer_cast<LibraryISO15765>(shared_from_this()), ret);
	mDevices.push_back(ret);
	return ret;
}

void LibraryISO15765::close(const DevicePtr &devicePtr) {
	mDevices.remove(devicePtr);
}

void LibraryISO15765::getLastError(char *pErrorDescription) {
    return mLibrary->getLastError(pErrorDescription);
}

DeviceISO15765::DeviceISO15765(const LibraryISO15765Ptr &library, const DevicePtr &device) : mLibrary(library), mDevice(device) {
}

DeviceISO15765::~DeviceISO15765() {
    LibraryPtr library = mDevice->getLibrary().lock();
    assert(library);
	
    library->close(mDevice);
}

ChannelPtr DeviceISO15765::connect(unsigned long ProtocolID, unsigned long Flags, unsigned long BaudRate) {
	UNUSED(Flags);
	UNUSED(BaudRate);
	if ((ProtocolID & 0x1FFF) == ISO15765) {
		ProtocolID--; // USE CAN instead
	}
	ChannelPtr ret = mDevice->connect(ProtocolID, Flags, BaudRate);
	if ((ProtocolID & 0x1FFF) == ISO15765) {
		ret = std::make_shared<ChannelISO15765>(std::static_pointer_cast<DeviceISO15765>(shared_from_this()), ret);
	}
	return ret;
}

void DeviceISO15765::disconnect(const ChannelPtr &channelPtr) {
    mChannels.remove(channelPtr);
}

void DeviceISO15765::setProgrammingVoltage(unsigned long PinNumber, unsigned long Voltage) {
    mDevice->setProgrammingVoltage(PinNumber, Voltage);
}

void DeviceISO15765::readVersion(char *pFirmwareVersion, char *pDllVersion, char *pApiVersion) {
    mDevice->readVersion(pFirmwareVersion, pDllVersion, pApiVersion);
}

void DeviceISO15765::ioctl(unsigned long IoctlID, void *pInput, void *pOutput) {
    mDevice->ioctl(IoctlID, pInput, pOutput);
}

LibraryWeakPtr DeviceISO15765::getLibrary() const {
    return mLibrary;
}

	
/*
 *
 * ISO15765Transfer
 *
 */
static uint32_t data2pid(const uint8_t *data) {
    uint32_t pid = 0;
    
    pid |= ((0x1F & data[0]) << 24);
    pid |= ((0xFF & data[1]) << 16);
    pid |= ((0xFF & data[2]) << 8);
    pid |= ((0xFF & data[3]) << 0);
    
    return pid;
}

static void pid2Data(uint32_t pid, uint8_t *data) {
    data[0] = (0x1F & (pid >> 24));
    data[1] = (0xFF & (pid >> 16));
    data[2] = (0xFF & (pid >> 8));
    data[3] = (0xFF & (pid >> 0));
}

ISO15765Transfer::ISO15765Transfer(Configuration &configuration, Channel &channel, const PASSTHRU_MSG &pMaskMsg, const PASSTHRU_MSG &pPatternMsg, const PASSTHRU_MSG &pFlowControlMsg): mChannelConfiguration(configuration), mChannel(channel), mState(START_STATE) {
    mMaskPid = data2pid(pMaskMsg.Data);
    mPatternPid = data2pid(pPatternMsg.Data);
    mFlowControlPid = data2pid(pFlowControlMsg.Data);
	clear();
}

ISO15765Transfer::~ISO15765Transfer() {
    
}

void ISO15765Transfer::clear() {
    mState = START_STATE;
    mOffset = 0;
	mSequence = 0;
	mBs = 0;
	mStmin = 0;
}

#define IS_SF(d) (((d & 0xF0) >> 4) == 0)
#define IS_FF(d) (((d & 0xF0) >> 4) == 1)
#define IS_CF(d) (((d & 0xF0) >> 4) == 2)
#define IS_FC(d) (((d & 0xF0) >> 4) == 3)
#define GET_MS(d) (d & 0x0F)

#define J2534_DATA_OFFSET 4
#define CAN_DATA_SIZE 8
#define J2534_PCI_SIZE 1
#define J2534_LENGTH_SIZE 1
#define J2534_BS_SIZE 1
#define J2534_STMIN_SIZE 1

ISO15765Transfer::PCIFrameName ISO15765Transfer::getFrameName(uint8_t pci) {
    if(IS_SF(pci)) {
        return SingleFrame;
    } else if(IS_FF(pci)) {
        return FirstFrame;
    } else if(IS_CF(pci)) {
        return ConsecutiveFrame;
    } else if(IS_FC(pci)) {
        return FlowControl;
    } else {
        return UnknownFrame;
    }
}

uint8_t ISO15765Transfer::getPci(PCIFrameName frameName) {
    if(frameName == SingleFrame) {
        return (0x0 << 4);
    } else if(frameName == FirstFrame) {
        return (0x1 << 4);
    } else if(frameName == ConsecutiveFrame) {
        return (0x2 << 4);
    } else if(frameName == FlowControl) {
        return (0x3 << 4);
    } else {
        return (0xf << 4);
    }
}

size_t ISO15765Transfer::getRemainingSize(const PASSTHRU_MSG &msg, off_t offset) {
    size_t ret = msg.DataSize - offset;
    if(ret > 7) {
        ret = 7;
    }
    return ret;
}

void ISO15765Transfer::prepareSentMessageHeaders(PASSTHRU_MSG &out_msg, const PASSTHRU_MSG &in_msg) {
    out_msg.ProtocolID = CAN;
    out_msg.RxStatus = 0;
    out_msg.TxFlags = in_msg.TxFlags & ~(ISO15765_FRAME_PAD|ISO15765_ADDR_TYPE);
    out_msg.Timestamp = 0;
    out_msg.DataSize = 0;
    out_msg.ExtraDataIndex = 0;
    
    // Copy the PID
    memcpy(&(out_msg.Data[0]), &(in_msg.Data[0]), J2534_DATA_OFFSET);
}

void ISO15765Transfer::prepareReceivedMessageHeaders(PASSTHRU_MSG &out_msg, const PASSTHRU_MSG &in_msg) {
    out_msg.ProtocolID = ISO15765;
    out_msg.RxStatus = in_msg.RxStatus;
    out_msg.TxFlags = 0;
    out_msg.Timestamp = 0;
    out_msg.DataSize = 0;
    out_msg.ExtraDataIndex = 0;
    
    // Copy the PID
    memcpy(&(out_msg.Data[0]), &(in_msg.Data[0]), J2534_DATA_OFFSET);
}

void ISO15765Transfer::paddingMessage(PASSTHRU_MSG &smsg) {
    for(int i = smsg.DataSize; i < CAN_DATA_SIZE + J2534_DATA_OFFSET; ++i) {
        smsg.Data[i] = '\0';
    }
    smsg.DataSize = CAN_DATA_SIZE + J2534_DATA_OFFSET;
}

bool ISO15765Transfer::writeMsg(const PASSTHRU_MSG &msg, unsigned long Timeout) {
    PASSTHRU_MSG &tmp_msg = mMessage;
    
    // Set Deadline
    std::chrono::time_point<std::chrono::steady_clock> deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    
    // Sanity checks
    if(msg.DataSize < J2534_DATA_OFFSET) {
         throw J2534Exception(ERR_INVALID_MSG);
    }
    
    if(mState != START_STATE) {
        LOG_DEBUG("Wrong state");
        goto fail;
    }
    
    while(msg.DataSize > (size_t)mOffset) {
        Timeout = (std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())).count();
        if(Timeout <= 0) {
            return false;
        }
        
        if(mState == START_STATE) {
            mOffset = J2534_DATA_OFFSET;
            prepareSentMessageHeaders(tmp_msg, msg);
            
            // Compute
            PCIFrameName frameName = SingleFrame;
            size_t size = getRemainingSize(msg, mOffset);
            
            if(size < (msg.DataSize - mOffset)) {
                frameName = FirstFrame;
            }
            
            // Fill the buffer
            if(frameName == FirstFrame) {
                size_t fullsize = msg.DataSize - mOffset;
                tmp_msg.Data[J2534_DATA_OFFSET] = (getPci(frameName) & 0xF0)| ((fullsize >> 8) & 0x0F);
                tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE] = (fullsize & 0xFF);
                size = CAN_DATA_SIZE - J2534_PCI_SIZE - J2534_LENGTH_SIZE;
                mSequence++;
                tmp_msg.DataSize = J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_LENGTH_SIZE + size;
                memcpy(&(tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_LENGTH_SIZE]), &(msg.Data[mOffset]), size);
            } else {
                tmp_msg.Data[J2534_DATA_OFFSET] = (getPci(frameName) & 0xF0)| (size & 0x0F);
                tmp_msg.DataSize = J2534_DATA_OFFSET + J2534_PCI_SIZE + size;
                memcpy(&(tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE]), &(msg.Data[mOffset]), size);
            }
            
            mOffset += size;
            
            // Padding
            if(msg.TxFlags & ISO15765_FRAME_PAD) {
                paddingMessage(tmp_msg);
            }
            
			unsigned long count = 1;
			mChannel.writeMsgs(&tmp_msg, &count, Timeout);
            if(count != 1) {
                LOG_DEBUG("Can't write message %d", frameName);
                goto fail;
            }
            mState = FLOW_CONTROL_STATE;
        } else if (mState == FLOW_CONTROL_STATE) {
			unsigned long count = 1;
			mChannel.readMsgs(&tmp_msg, &count, Timeout);
            if(count != 1) {
                LOG_DEBUG("Can't read flow control message");
                goto fail;
            }
            if(tmp_msg.DataSize < J2534_DATA_OFFSET) {
                LOG_DEBUG("Invalid flow control message size");
                goto fail;
            }
            if((data2pid(tmp_msg.Data) & mMaskPid) != mPatternPid) {
                LOG_DEBUG("Incorrect PID");
                goto fail;
            }
            PCIFrameName frameName = getFrameName(tmp_msg.Data[J2534_DATA_OFFSET]);
            if(frameName != FlowControl) {
                LOG_DEBUG("Invalid frame type %d (Need %d)", frameName, FlowControl);
                goto fail;
            }
            
            // Get block information
            mBs = tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE];
            mStmin = tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_BS_SIZE];
            
            std::this_thread::sleep_for(std::chrono::milliseconds(mStmin));
            
            mState = BLOCK_STATE;
        } else if (mState == BLOCK_STATE) {
            prepareSentMessageHeaders(tmp_msg, msg);
            
            // Compute
            PCIFrameName frameName = ConsecutiveFrame;
            size_t size = getRemainingSize(msg, mOffset);
            
            // Fill the buffer
            tmp_msg.Data[J2534_DATA_OFFSET] = (getPci(frameName) & 0xF0)| ((mSequence++) & 0x0F);
            tmp_msg.DataSize = J2534_DATA_OFFSET + J2534_PCI_SIZE + size;
            memcpy(&(tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE]), &(msg.Data[mOffset]), size);
            
            mOffset += size;
            
            // Padding
            if(msg.TxFlags & ISO15765_FRAME_PAD) {
                paddingMessage(tmp_msg);
            }
            
            // Write the message
			unsigned long count = 1;
			mChannel.writeMsgs(&tmp_msg, &count, Timeout);
            if(count != 1) {
                LOG_DEBUG("Can't write message");
                goto fail;
            }
            
            // End of the block ?
            if(--mBs == 0) {
                mState = FLOW_CONTROL_STATE;
            }
            
            if(mState == BLOCK_STATE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(mStmin));
            }
        } else {
            LOG_DEBUG("Wrong state");
            goto fail;
        }
    }
    
    clear();
    return true;
    
fail:
    clear();
    return false;
}

bool ISO15765Transfer::readMsg(const PASSTHRU_MSG &in_msg, PASSTHRU_MSG &out_msg, unsigned long Timeout) {
    PASSTHRU_MSG &read_msg = mMessage;
    if(in_msg.DataSize < J2534_DATA_OFFSET) {
        LOG_DEBUG("Invalid flow control message size");
        goto fail;
    }
    if((data2pid(in_msg.Data) & mMaskPid) != mPatternPid) {
        LOG_DEBUG("Incorrect PID");
        goto fail;
    }
    {
        PCIFrameName frameName = getFrameName(in_msg.Data[J2534_DATA_OFFSET]);
        if(mState == START_STATE) {
            prepareReceivedMessageHeaders(read_msg, in_msg);
            mOffset = J2534_DATA_OFFSET;
            
            if(frameName == SingleFrame) {
                size_t size = in_msg.Data[J2534_DATA_OFFSET] & 0x0F;
                read_msg.DataSize = J2534_DATA_OFFSET + size;
                memcpy(&(read_msg.Data[mOffset]), &(in_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE]), size);
                
                mOffset += size;
            } else if(frameName == FirstFrame) {
                size_t fullsize = ((in_msg.Data[J2534_DATA_OFFSET] & 0x0F) << 8) | (in_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE] & 0xFF);
                read_msg.DataSize = J2534_DATA_OFFSET + fullsize;
                size_t size = CAN_DATA_SIZE - J2534_PCI_SIZE - J2534_LENGTH_SIZE;
                memcpy(&(read_msg.Data[mOffset]), &(in_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_LENGTH_SIZE]), size);
                
                mSequence++;
                mOffset += size;
                
                if(!sendFlowControlMessage(Timeout)) {
                    LOG_DEBUG("Can't send flow control message");
                    goto fail;
                }

                mState = BLOCK_STATE;
            } else {
                LOG_DEBUG("Invalid frame type %d", frameName);
                goto fail;
            }
        } else if(mState == BLOCK_STATE) {
            unsigned int seq = (in_msg.Data[J2534_DATA_OFFSET]) & 0xF;
            if (seq != (mSequence % 0x10)) {
                LOG_DEBUG("Wrong sequence number %d (Need %d)", seq, mSequence);
                goto fail;
            }
            
            size_t size = getRemainingSize(read_msg, mOffset);
            memcpy(&(read_msg.Data[mOffset]), &(in_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE]), size);
            
            mSequence++;
            mOffset += size;
            
            if(--mBs == 0) {
                if(!sendFlowControlMessage(Timeout)) {
                    LOG_DEBUG("Can't send flow control message");
                    goto fail;
                }
            }
        } else {
            LOG_DEBUG("Wrong state");
            goto fail;
        }
        
        if((size_t)mOffset >= read_msg.DataSize) {
            memcpy(&out_msg, &read_msg, sizeof(PASSTHRU_MSG));
            clear();
            return true;
        }
    }
    return false;
    
fail:
    clear();
    return false;
}

bool ISO15765Transfer::sendFlowControlMessage(unsigned long Timeout) {
    PASSTHRU_MSG tmp_msg;
	
	mBs = 0;
    mChannelConfiguration.getValue(ISO15765_BS, &mBs);
	mStmin = 0;
    mChannelConfiguration.getValue(ISO15765_STMIN, &mStmin);
    
    tmp_msg.ProtocolID = CAN;
    tmp_msg.RxStatus = 0;
    tmp_msg.TxFlags = 0;
    tmp_msg.Timestamp = 0;
    tmp_msg.DataSize = J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_BS_SIZE + J2534_STMIN_SIZE;
    tmp_msg.ExtraDataIndex = 0;
    
    pid2Data(mFlowControlPid, tmp_msg.Data);
    tmp_msg.Data[J2534_DATA_OFFSET] = getPci(FlowControl);
    tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE] = mBs;
    tmp_msg.Data[J2534_DATA_OFFSET + J2534_PCI_SIZE + J2534_BS_SIZE] = mStmin;
    paddingMessage(tmp_msg);
    
	unsigned long count = 1;
	mChannel.writeMsgs(&tmp_msg, &count, Timeout);
    if(count != 1) {
        return false;
    }
    return true;
}

uint32_t ISO15765Transfer::getMaskPid() {
    return mMaskPid;
}

uint32_t ISO15765Transfer::getPatternPid() {
    return mPatternPid;
}

uint32_t ISO15765Transfer::getFlowControlPid() {
    return mFlowControlPid;
}

/*
 *
 * ChannelISO15765
 *
 */

ChannelISO15765::ChannelISO15765(const DeviceISO15765Ptr &device, const ChannelPtr &channel): ConfigurableChannel(ISO15765), mDevice(device), mChannel(channel) {
    
}

ChannelISO15765::~ChannelISO15765() {
    
}

std::shared_ptr<ISO15765Transfer> ChannelISO15765::getTransferByFlowControl(const PASSTHRU_MSG &msg) {
    uint32_t pid = data2pid(msg.Data);
    auto it = std::find_if(mMessageFilters.begin(), mMessageFilters.end(), [&](const std::shared_ptr<MessageFilterISO15765> &messageFilter) {
        return messageFilter->getTransfer()->getFlowControlPid() == pid;
    });
    if (it != mMessageFilters.end())  {
        return (*it)->getTransfer();
    }
    return nullptr;
}

std::shared_ptr<ISO15765Transfer> ChannelISO15765::getTransferByPattern(const PASSTHRU_MSG &msg) {
    uint32_t pid = data2pid(msg.Data);
    auto it = std::find_if(mMessageFilters.begin(), mMessageFilters.end(), [&](const std::shared_ptr<MessageFilterISO15765> &messageFilter) {
        return messageFilter->getTransfer()->getPatternPid() == (pid & messageFilter->getTransfer()->getMaskPid());
    });
    if (it != mMessageFilters.end())  {
        return (*it)->getTransfer();
    }
    return nullptr;
}

MessageFilterPtr ChannelISO15765::startMsgFilter(unsigned long FilterType, PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                     PASSTHRU_MSG *pFlowControlMsg) {
    if(FilterType == FLOW_CONTROL_FILTER) {
        if (pMaskMsg == NULL || pPatternMsg == NULL || pFlowControlMsg == NULL) {
            throw J2534Exception(ERR_NULLPARAMETER);
        }
        PASSTHRU_MSG maskMsg, patternMsg;
        maskMsg = *pMaskMsg;
        patternMsg = *pPatternMsg;
        maskMsg.ProtocolID = patternMsg.ProtocolID = CAN;
        maskMsg.RxStatus &= ~(ISO15765_PADDING_ERROR | ISO15765_ADDR_TYPE);
        maskMsg.TxFlags &=~(ISO15765_FRAME_PAD);
        patternMsg.RxStatus &= ~(ISO15765_PADDING_ERROR | ISO15765_ADDR_TYPE);
        patternMsg.TxFlags &=~(ISO15765_FRAME_PAD);
        
        MessageFilterISO15765Ptr mf = std::make_shared<MessageFilterISO15765>(std::static_pointer_cast<ChannelISO15765>(shared_from_this()),
			mChannel->startMsgFilter(PASS_FILTER, &maskMsg, &patternMsg, NULL),
			std::make_shared<ISO15765Transfer>(getConfiguration(), *mChannel, *pMaskMsg, *pPatternMsg, *pFlowControlMsg));
		mMessageFilters.push_back(mf);
		return mf;
	} else {
        return mChannel->startMsgFilter(FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg);
    }
}

void ChannelISO15765::stopMsgFilter(const MessageFilterPtr &messageFilter) {
	auto it = std::find(mMessageFilters.begin(), mMessageFilters.end(), messageFilter);
	if(it != mMessageFilters.end()) {
		mMessageFilters.erase(it);
	} else {
		mChannel->stopMsgFilter(messageFilter);
	}
}

void ChannelISO15765::readMsgs(PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout) {
    unsigned long count = 0;
	
	PASSTHRU_MSG readMsg;
    
    std::chrono::time_point<std::chrono::steady_clock> deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    for(unsigned long i = 0; i < *pNumMsgs; ++i) {
        while(true) {
            Timeout = (std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())).count();
            if(Timeout <= 0) {
                LOG_DEBUG("Timeout");
				goto end;
            }
            
			{
				unsigned long c = 1;
				mChannel->readMsgs(&readMsg, &c, Timeout);
				if (c != 1) {
					LOG_DEBUG("Can't read msg");
					goto end;
				}
			}
            
            Timeout = (std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())).count();

            // Get transfer
            auto transfer = getTransferByPattern(readMsg);
            if (transfer) {
                if(transfer->readMsg(readMsg, *pMsg, Timeout)) {
					count++;
                    break;
                }
            } else {
                LOG_DEBUG("No matching transfer");
            }
        }
    };
	
end:
	*pNumMsgs = count;
}

void ChannelISO15765::writeMsgs(PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout) {
    unsigned long count = 0;
	
    std::chrono::time_point<std::chrono::steady_clock> deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    for(unsigned long i = 0; i < *pNumMsgs; ++i) {
        Timeout = (std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())).count();
        if(Timeout <= 0) {
			goto end;
        }
		
		PASSTHRU_MSG &msg = *(pMsg++);
        auto transfer = getTransferByFlowControl(msg);
        if (transfer) {
            if(transfer->writeMsg(msg, Timeout)) {
                count++;
            } else {
                LOG_DEBUG("Can't write msg");
            }
        } else {
            LOG_DEBUG("Ignore msg");
        }
    };
	
end:
    *pNumMsgs = count;
}

PeriodicMessagePtr ChannelISO15765::startPeriodicMsg(PASSTHRU_MSG *pMsg, unsigned long TimeInterval) {
    return mChannel->startPeriodicMsg(pMsg, TimeInterval);
}

void ChannelISO15765::stopPeriodicMsg(const PeriodicMessagePtr &periodicMessage) {
    mChannel->stopPeriodicMsg(periodicMessage);
}

DeviceWeakPtr ChannelISO15765::getDevice() const {
    return mDevice;
}

bool ChannelISO15765::getConfig(SCONFIG *config) const {
	ConfigurableChannel::getConfig(config);
	unsigned long parameter = config->Parameter;
	if(parameter != ISO15765_BS && parameter != ISO15765_STMIN && parameter != ISO15765_ADDR_TYPE) {
		SCONFIG_LIST Input;
		Input.NumOfParams = 1;
		Input.ConfigPtr = config;
		mChannel->ioctl(GET_CONFIG, &Input, NULL);
	}
    return true;
}

bool ChannelISO15765::setConfig(SCONFIG *config) {
	ConfigurableChannel::setConfig(config);
	unsigned long parameter = config->Parameter;
	if(parameter != ISO15765_BS && parameter != ISO15765_STMIN && parameter != ISO15765_ADDR_TYPE) {
		SCONFIG_LIST Input;
		Input.NumOfParams = 1;
		Input.ConfigPtr = config;
		mChannel->ioctl(SET_CONFIG, &Input, NULL);
	}
    return true;
}
	
bool ChannelISO15765::clearTxBuffers() {
    return false;
}

bool ChannelISO15765::clearRxBuffers() {
    return false;
}

bool ChannelISO15765::clearPeriodicMessages() {
    return false;
}

bool ChannelISO15765::clearMessageFilters() {
	mMessageFilters.clear();
    return false;
}
	
bool ChannelISO15765::handle_ioctl(unsigned long IoctlID, void *pInput, void *pOutput) {
    if(!ConfigurableChannel::handle_ioctl(IoctlID, pInput, pOutput)) {
	    mChannel->ioctl(IoctlID, pInput, pOutput);
	}
	return true;
}


MessageFilterISO15765::MessageFilterISO15765(const ChannelISO15765Ptr &channel, const MessageFilterPtr &messageFilter, const ISO15765TransferPtr &transfer): mChannel(channel), mMessageFilter(messageFilter), mTransfer(transfer) {

}

MessageFilterISO15765::~MessageFilterISO15765() {
	ChannelPtr channel = mMessageFilter->getChannel().lock();
	assert(channel);
	
	channel->stopMsgFilter(mMessageFilter);
}

ChannelWeakPtr MessageFilterISO15765::getChannel() const {
	return mChannel;
}
	
ISO15765TransferPtr& MessageFilterISO15765::getTransfer() {
	return mTransfer;
}