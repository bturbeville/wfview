#include "rigcommander.h"
#include <QDebug>

#include "rigidentities.h"
#include "logcategories.h"
#include "printhex.h"

// Copyright 2017-2020 Elliott H. Liggett

// This file parses data from the radio and also forms commands to the radio.
// The radio physical interface is handled by the commHandler() instance "comm"

//
// See here for a wonderful CI-V overview:
// http://www.plicht.de/ekki/civ/civ-p0a.html
//
// The IC-7300 "full" manual also contains a command reference.

// How to make spectrum display stop using rigctl:
//  echo "w \0xFE\0xFE\0x94\0xE0\0x27\0x11\0x00\0xFD" | rigctl -m 3073 -r /dev/ttyUSB0 -s 115200 -vvvvv

// Note: When sending \x00, must use QByteArray.setRawData()

rigCommander::rigCommander(QObject* parent) : QObject(parent)
{
    qInfo(logRig()) << "creating instance of rigCommander()";
    state.set(SCOPEFUNC, true, false);
}

rigCommander::rigCommander(quint8 guid[GUIDLEN], QObject* parent) : QObject(parent)
{
    qInfo(logRig()) << "creating instance of rigCommander()";
    state.set(SCOPEFUNC, true, false);
    memcpy(this->guid, guid, GUIDLEN);
}

rigCommander::~rigCommander()
{
    qInfo(logRig()) << "closing instance of rigCommander()";
    closeComm();
}


void rigCommander::commSetup(unsigned char rigCivAddr, QString rigSerialPort, quint32 rigBaudRate, QString vsp,quint16 tcpPort, quint8 wf)
{
    // construct
    // TODO: Bring this parameter and the comm port from the UI.
    // Keep in hex in the UI as is done with other CIV apps.

    // civAddr = 0x94; // address of the radio. Decimal is 148.
    civAddr = rigCivAddr; // address of the radio. Decimal is 148.
    usingNativeLAN = false;

    //qInfo(logRig()) << "Opening connection to Rig:" << QString("0x%1").arg((unsigned char)rigCivAddr,0,16) << "on serial port" << rigSerialPort << "at baud rate" << rigBaudRate;
    // ---
    setup();
    // ---

    this->rigSerialPort = rigSerialPort;
    this->rigBaudRate = rigBaudRate;
    rigCaps.baudRate = rigBaudRate;

    comm = new commHandler(rigSerialPort, rigBaudRate,wf,this);
    ptty = new pttyHandler(vsp,this);

    if (tcpPort > 0) {
        tcp = new tcpServer(this);
        tcp->startServer(tcpPort);
    }


    // data from the comm port to the program:
    connect(comm, SIGNAL(haveDataFromPort(QByteArray)), this, SLOT(handleNewData(QByteArray)));

    // data from the ptty to the rig:
    connect(ptty, SIGNAL(haveDataFromPort(QByteArray)), comm, SLOT(receiveDataFromUserToRig(QByteArray)));

    // data from the program to the comm port:
    connect(this, SIGNAL(dataForComm(QByteArray)), comm, SLOT(receiveDataFromUserToRig(QByteArray)));
    if (tcpPort > 0) {
        // data from the tcp port to the rig:
        connect(tcp, SIGNAL(receiveData(QByteArray)), comm, SLOT(receiveDataFromUserToRig(QByteArray)));
        connect(comm, SIGNAL(haveDataFromPort(QByteArray)), tcp, SLOT(sendData(QByteArray)));
    }
    connect(this, SIGNAL(toggleRTS(bool)), comm, SLOT(setRTS(bool)));

    // data from the rig to the ptty:
    connect(comm, SIGNAL(haveDataFromPort(QByteArray)), ptty, SLOT(receiveDataFromRigToPtty(QByteArray)));

    connect(comm, SIGNAL(havePortError(errorType)), this, SLOT(handlePortError(errorType)));
    connect(ptty, SIGNAL(havePortError(errorType)), this, SLOT(handlePortError(errorType)));

    connect(this, SIGNAL(getMoreDebug()), comm, SLOT(debugThis()));
    connect(this, SIGNAL(getMoreDebug()), ptty, SLOT(debugThis()));

    connect(this, SIGNAL(discoveredRigID(rigCapabilities)), ptty, SLOT(receiveFoundRigID(rigCapabilities)));

    emit commReady();
    sendState(); // Send current rig state to rigctld

}

void rigCommander::commSetup(unsigned char rigCivAddr, udpPreferences prefs, audioSetup rxSetup, audioSetup txSetup, QString vsp, quint16 tcpPort)
{
    // construct
    // TODO: Bring this parameter and the comm port from the UI.
    // Keep in hex in the UI as is done with other CIV apps.

    // civAddr = 0x94; // address of the radio. Decimal is 148.
    civAddr = rigCivAddr; // address of the radio. Decimal is 148.
    usingNativeLAN = true;
    // -- 
    setup();
    // ---


    if (udp == Q_NULLPTR) {

        udp = new udpHandler(prefs,rxSetup,txSetup);

        udpHandlerThread = new QThread(this);
        udpHandlerThread->setObjectName("udpHandler()");

        udp->moveToThread(udpHandlerThread);

        connect(this, SIGNAL(initUdpHandler()), udp, SLOT(init()));
        connect(udpHandlerThread, SIGNAL(finished()), udp, SLOT(deleteLater()));

        udpHandlerThread->start();

        emit initUdpHandler();

        //this->rigSerialPort = rigSerialPort;
        //this->rigBaudRate = rigBaudRate;

        ptty = new pttyHandler(vsp,this);

        if (tcpPort > 0) {
            tcp = new tcpServer(this);
            tcp->startServer(tcpPort);
        }
        // Data from UDP to the program
        connect(udp, SIGNAL(haveDataFromPort(QByteArray)), this, SLOT(handleNewData(QByteArray)));

        // data from the rig to the ptty:
        connect(udp, SIGNAL(haveDataFromPort(QByteArray)), ptty, SLOT(receiveDataFromRigToPtty(QByteArray)));

        // Audio from UDP
        connect(udp, SIGNAL(haveAudioData(audioPacket)), this, SLOT(receiveAudioData(audioPacket)));

        // data from the program to the rig:
        connect(this, SIGNAL(dataForComm(QByteArray)), udp, SLOT(receiveDataFromUserToRig(QByteArray)));

        // data from the ptty to the rig:
        connect(ptty, SIGNAL(haveDataFromPort(QByteArray)), udp, SLOT(receiveDataFromUserToRig(QByteArray)));

        if (tcpPort > 0) {
            // data from the tcp port to the rig:
            connect(tcp, SIGNAL(receiveData(QByteArray)), udp, SLOT(receiveDataFromUserToRig(QByteArray)));
            connect(udp, SIGNAL(haveDataFromPort(QByteArray)), tcp, SLOT(sendData(QByteArray)));
        }

        connect(this, SIGNAL(haveChangeLatency(quint16)), udp, SLOT(changeLatency(quint16)));
        connect(this, SIGNAL(haveSetVolume(unsigned char)), udp, SLOT(setVolume(unsigned char)));
        connect(udp, SIGNAL(haveBaudRate(quint32)), this, SLOT(receiveBaudRate(quint32)));

        // Connect for errors/alerts
        connect(udp, SIGNAL(haveNetworkError(errorType)), this, SLOT(handlePortError(errorType)));
        connect(udp, SIGNAL(haveNetworkStatus(networkStatus)), this, SLOT(handleStatusUpdate(networkStatus)));
        connect(udp, SIGNAL(haveNetworkAudioLevels(networkAudioLevels)), this, SLOT(handleNetworkAudioLevels(networkAudioLevels)));


        connect(ptty, SIGNAL(havePortError(errorType)), this, SLOT(handlePortError(errorType)));
        connect(this, SIGNAL(getMoreDebug()), ptty, SLOT(debugThis()));

        connect(this, SIGNAL(discoveredRigID(rigCapabilities)), ptty, SLOT(receiveFoundRigID(rigCapabilities)));

        connect(udp, SIGNAL(requestRadioSelection(QList<radio_cap_packet>)), this, SLOT(radioSelection(QList<radio_cap_packet>)));
        connect(udp, SIGNAL(setRadioUsage(quint8, quint8, QString, QString)), this, SLOT(radioUsage(quint8, quint8, QString, QString)));
        connect(this, SIGNAL(selectedRadio(quint8)), udp, SLOT(setCurrentRadio(quint8)));
        emit haveAfGain(rxSetup.localAFgain);
        localVolume = rxSetup.localAFgain;
    }

    // data from the comm port to the program:

    emit commReady();
    sendState(); // Send current rig state to rigctld

    pttAllowed = true; // This is for developing, set to false for "safe" debugging. Set to true for deployment.

}

void rigCommander::closeComm()
{
    qDebug(logRig()) << "Closing rig comms";
    if (comm != Q_NULLPTR) {
        delete comm;
    }
    comm = Q_NULLPTR;

    if (udpHandlerThread != Q_NULLPTR) {
        udpHandlerThread->quit();
        udpHandlerThread->wait();
    }
    udp = Q_NULLPTR;

    if (ptty != Q_NULLPTR) {
        delete ptty;
    }
    ptty = Q_NULLPTR;
}

void rigCommander::setup()
{
    // common elements between the two constructors go here:
    setCIVAddr(civAddr);
    spectSeqMax = 0; // this is now set after rig ID determined

    payloadSuffix = QByteArray("\xFD");

    lookingForRig = false;
    foundRig = false;
    oldScopeMode = spectModeUnknown;

    pttAllowed = true; // This is for developing, set to false for "safe" debugging. Set to true for deployment.
}



void rigCommander::process()
{
    // new thread enters here. Do nothing but do check for errors.
    if(comm!=Q_NULLPTR && comm->serialError)
    {
        emit havePortError(errorType(rigSerialPort, QString("Error from commhandler. Check serial port.")));
    }
}

void rigCommander::handlePortError(errorType err)
{
    qInfo(logRig()) << "Error using port " << err.device << " message: " << err.message;
    emit havePortError(err);
}

void rigCommander::handleStatusUpdate(const networkStatus status)
{
    emit haveStatusUpdate(status);
}

void rigCommander::handleNetworkAudioLevels(networkAudioLevels l)
{
    emit haveNetworkAudioLevels(l);
}

bool rigCommander::usingLAN()
{
    return usingNativeLAN;
}

void rigCommander::receiveBaudRate(quint32 baudrate) {
    rigCaps.baudRate = baudrate;
    emit haveBaudRate(baudrate);
}

void rigCommander::setRTSforPTT(bool enabled)
{
    if(!usingNativeLAN)
    {
        useRTSforPTT_isSet = true;
        useRTSforPTT_manual = enabled;
        if(comm != NULL)
        {
            rigCaps.useRTSforPTT=enabled;
            comm->setUseRTSforPTT(enabled);
        }
    }
}

void rigCommander::findRigs()
{
    // This function sends data to 0x00 ("broadcast") to look for any connected rig.
    lookingForRig = true;
    foundRig = false;

    QByteArray data;
    QByteArray data2;
    //data.setRawData("\xFE\xFE\xa2", 3);
    data.setRawData("\xFE\xFE\x00", 3);
    data.append((char)compCivAddr); // wfview's address, 0xE1
    data2.setRawData("\x19\x00", 2); // get rig ID
    data.append(data2);
    data.append(payloadSuffix);

    emit dataForComm(data);
    // HACK for testing radios that do not respond to rig ID queries: 
    //this->model = model736;
    //this->determineRigCaps();
    return;
}

void rigCommander::prepDataAndSend(QByteArray data)
{
    data.prepend(payloadPrefix);
    //printHex(data, false, true);
    data.append(payloadSuffix);

    if(data[4] != '\x15')
    {
        // We don't print out requests for meter levels
        qDebug(logRigTraffic()) << "Final payload in rig commander to be sent to rig: ";
        //printHex(data);
        //printHex(data, logRigTraffic());
        printHexNow(data, logRigTraffic());
    }

    emit dataForComm(data);
}

void rigCommander::powerOn()
{
    QByteArray payload;

    int numFE=150;
    switch (this->rigBaudRate) {
    case 57600:
        numFE = 75;
        break;
    case 38400:
        numFE = 50;
        break;
    case 19200:
        numFE = 25;
        break;
    case 9600:
        numFE = 13;
        break;
    case 4800:
        numFE = 7;
        break;
    }

    for(int i=0; i < numFE; i++)
    {
        payload.append("\xFE");
    }

    payload.append(payloadPrefix); // FE FE 94 E1
    payload.append("\x18\x01");
    payload.append(payloadSuffix); // FD

    qDebug(logRig()) << "Power ON command in rigcommander to be sent to rig: ";
    printHex(payload);

    emit dataForComm(payload);

}

void rigCommander::powerOff()
{
    QByteArray payload;
    payload.setRawData("\x18\x00", 2);
    prepDataAndSend(payload);
}

void rigCommander::enableSpectOutput()
{
    QByteArray payload("\x27\x11\x01");
    prepDataAndSend(payload);
}

void rigCommander::disableSpectOutput()
{
    QByteArray payload;
    payload.setRawData("\x27\x11\x00", 3);
    prepDataAndSend(payload);
}

void rigCommander::enableSpectrumDisplay()
{
    // 27 10 01
    QByteArray payload("\x27\x10\x01");
    prepDataAndSend(payload);
}

void rigCommander::disableSpectrumDisplay()
{
    // 27 10 00
    QByteArray payload;
    payload.setRawData("\x27\x10\x00", 3);
    prepDataAndSend(payload);
}

void rigCommander::setSpectrumBounds(double startFreq, double endFreq, unsigned char edgeNumber)
{
    if((edgeNumber > 4) || (!edgeNumber))
    {
        return;
    }

    unsigned char freqRange = 1; // 1 = VHF, 2 = UHF, 3 = L-Band

    switch(rigCaps.model)
    {
        case model9700:
            if(startFreq > 148)
            {
                freqRange++;
                if(startFreq > 450)
                {
                    freqRange++;
                }
            }
            break;
        case model705:
        case model7300:
        case model7610:
        case model7850:
            // Some rigs do not go past 60 MHz, but we will not encounter
            // requests for those ranges since they are derived from the rig's existing scope range.
            // start value of freqRange is 1.
            if(startFreq > 1.6)
                freqRange++;
            if(startFreq > 2.0)
                freqRange++;
            if(startFreq > 6.0)
                freqRange++;
            if(startFreq > 8.0)
                freqRange++;
            if(startFreq > 11.0)
                freqRange++;
            if(startFreq > 15.0)
                freqRange++;
            if(startFreq > 20.0)
                freqRange++;
            if(startFreq > 22.0)
                freqRange++;
            if(startFreq > 26.0)
                freqRange++;
            if(startFreq > 30.0)
                freqRange++;
            if(startFreq > 45.0)
                freqRange++;
            if(startFreq > 60.0)
                freqRange++;
            if(startFreq > 74.8)
                freqRange++;
            if(startFreq > 108.0)
                freqRange++;
            if(startFreq > 137.0)
                freqRange++;
            if(startFreq > 400.0)
                freqRange++;
            break;
        case modelR8600:
            freqRange = 1;
            edgeNumber = 1;
            break;
        default:
            return;
            break;


    }
    QByteArray lowerEdge = makeFreqPayload(startFreq);
    QByteArray higherEdge = makeFreqPayload(endFreq);


    QByteArray payload;

    payload.setRawData("\x27\x1E", 2);
    payload.append(freqRange);
    payload.append(edgeNumber);

    payload.append(lowerEdge);
    payload.append(higherEdge);

    prepDataAndSend(payload);
}

void rigCommander::getScopeMode()
{
    // center or fixed
    QByteArray payload;
    payload.setRawData("\x27\x14\x00", 3);
    prepDataAndSend(payload);
}

void rigCommander::getScopeEdge()
{
    QByteArray payload;
    payload.setRawData("\x27\x16", 2);
    prepDataAndSend(payload);
}

void rigCommander::setScopeEdge(char edge)
{
    // 1 2 or 3
    // 27 16 00 0X
    if((edge <1) || (edge >4))
        return;
    QByteArray payload;
    payload.setRawData("\x27\x16\x00", 3);
    payload.append(edge);
    prepDataAndSend(payload);
}

void rigCommander::getScopeSpan()
{
    getScopeSpan(false);
}

void rigCommander::getScopeSpan(bool isSub)
{
    QByteArray payload;
    payload.setRawData("\x27\x15", 2);
    payload.append(static_cast<unsigned char>(isSub));
    prepDataAndSend(payload);
}

void rigCommander::setScopeSpan(char span)
{
    // See ICD, page 165, "19-12".
    // 2.5k = 0
    // 5k = 2, etc.
    if((span <0 ) || (span >9))
            return;

    QByteArray payload;
    double freq; // MHz
    payload.setRawData("\x27\x15\x00", 3);
    // next 6 bytes are the frequency
    switch(span)
    {
        case 0:
            // 2.5k
            freq = 2.5E-3;
            break;
        case 1:
            // 5k
            freq = 5.0E-3;
            break;
        case 2:
            freq = 10.0E-3;
            break;
        case 3:
            freq = 25.0E-3;
            break;
        case 4:
            freq = 50.0E-3;
            break;
        case 5:
            freq = 100.0E-3;
            break;
        case 6:
            freq = 250.0E-3;
            break;
        case 7:
            freq = 500.0E-3;
            break;
        case 8:
            freq = 1000.0E-3;
            break;
        case 9:
            freq = 2500.0E-3;
            break;
        default:
            return;
            break;
    }

    payload.append( makeFreqPayload(freq));
    payload.append("\x00");
    // printHex(payload, false, true);
    prepDataAndSend(payload);
}

void rigCommander::setSpectrumMode(spectrumMode spectMode)
{
    QByteArray specModePayload;
    specModePayload.setRawData("\x27\x14\x00", 3);
    specModePayload.append( static_cast<unsigned char>(spectMode) );
    prepDataAndSend(specModePayload);
}

void rigCommander::getSpectrumRefLevel()
{
    QByteArray payload;
    payload.setRawData("\x27\x19\x00", 3);
    prepDataAndSend(payload);
}

void rigCommander::getSpectrumRefLevel(unsigned char mainSub)
{
    QByteArray payload;
    payload.setRawData("\x27\x19", 2);
    payload.append(mainSub);
    prepDataAndSend(payload);
}

void rigCommander::setSpectrumRefLevel(int level)
{
    //qInfo(logRig()) << __func__ << ": Setting scope to level " << level;
    QByteArray setting;
    QByteArray number;
    QByteArray pn;
    setting.setRawData("\x27\x19\x00", 3);

    if(level >= 0)
    {
        pn.setRawData("\x00", 1);
        number = bcdEncodeInt(level*10);
    } else {
        pn.setRawData("\x01", 1);
        number = bcdEncodeInt( (-level)*10 );
    }

    setting.append(number);
    setting.append(pn);

    //qInfo(logRig()) << __func__ << ": scope reference number: " << number << ", PN to: " << pn;
    //printHex(setting, false, true);

    prepDataAndSend(setting);
}


void rigCommander::getSpectrumCenterMode()
{
    QByteArray specModePayload;
    specModePayload.setRawData("\x27\x14", 2);
    prepDataAndSend(specModePayload);
}

void rigCommander::getSpectrumMode()
{
    QByteArray specModePayload;
    specModePayload.setRawData("\x27\x14", 2);
    prepDataAndSend(specModePayload);
}

void rigCommander::setFrequency(unsigned char vfo, freqt freq)
{
    QByteArray freqPayload = makeFreqPayload(freq);
    QByteArray cmdPayload;

    cmdPayload.append(freqPayload);
    if (vfo == 0) {
        cmdPayload.prepend('\x00');
    }
    else
    {   
        cmdPayload.prepend(vfo);
        cmdPayload.prepend('\x25');
    }
    //printHex(cmdPayload, false, true);
    prepDataAndSend(cmdPayload);
}

void rigCommander::selectVFO(vfo_t vfo)
{
    // Note, some radios use main/sub,
    // some use A/B,
    // and some appear to use both...
    QByteArray payload;

    char vfoBytes[1];
    vfoBytes[0] = (unsigned char)vfo;

    payload.setRawData("\x07", 1);
    payload.append(vfoBytes, 1);
    prepDataAndSend(payload);
}

void rigCommander::equalizeVFOsAB()
{
    QByteArray payload;
    payload.setRawData("\x07\xA0", 2);
    prepDataAndSend(payload);
}

void rigCommander::equalizeVFOsMS()
{
    QByteArray payload;
    payload.setRawData("\x07\xB1", 2);
    prepDataAndSend(payload);
}

void rigCommander::exchangeVFOs()
{
    // NB: This command exchanges A-B or M-S
    // depending upon the radio.
    QByteArray payload;
    payload.setRawData("\x07\xB0", 2);
    prepDataAndSend(payload);
}

QByteArray rigCommander::makeFreqPayload(freqt freq)
{
    QByteArray result;
    quint64 freqInt = freq.Hz;

    unsigned char a;
    int numchars = 5;
    for (int i = 0; i < numchars; i++) {
        a = 0;
        a |= (freqInt) % 10;
        freqInt /= 10;
        a |= ((freqInt) % 10)<<4;

        freqInt /= 10;

        result.append(a);
        //printHex(result, false, true);
    }

    return result;
}

QByteArray rigCommander::makeFreqPayload(double freq)
{
    quint64 freqInt = (quint64) (freq * 1E6);

    QByteArray result;
    unsigned char a;
    int numchars = 5;
    for (int i = 0; i < numchars; i++) {
        a = 0;
        a |= (freqInt) % 10;
        freqInt /= 10;
        a |= ((freqInt) % 10)<<4;

        freqInt /= 10;

        result.append(a);
        //printHex(result, false, true);
    }
    //qInfo(logRig()) << "encoded frequency for " << freq << " as int " << freqInt;
    //printHex(result, false, true);
    return result;
}

void rigCommander::setRitEnable(bool ritEnabled)
{
    QByteArray payload;

    if(ritEnabled)
    {
        payload.setRawData("\x21\x01\x01", 3);
    } else {
        payload.setRawData("\x21\x01\x00", 3);
    }
    prepDataAndSend(payload);
}

void rigCommander::getRitEnabled()
{
    QByteArray payload;
    payload.setRawData("\x21\x01", 2);
    prepDataAndSend(payload);
}

void rigCommander::getRitValue()
{
    QByteArray payload;
    payload.setRawData("\x21\x00", 2);
    prepDataAndSend(payload);
}

void rigCommander::setRitValue(int ritValue)
{
    QByteArray payload;
    QByteArray freqBytes;
    freqt f;

    bool isNegative = false;
    payload.setRawData("\x21\x00", 2);

    if(ritValue < 0)
    {
        isNegative = true;
        ritValue *= -1;
    }

    if(ritValue > 9999)
        return;

    f.Hz = ritValue;

    freqBytes = makeFreqPayload(f);

    freqBytes.truncate(2);
    payload.append(freqBytes);

    payload.append(QByteArray(1,(char)isNegative));

    prepDataAndSend(payload);
}

void rigCommander::setMode(mode_info m)
{
    QByteArray payload;

    if (rigCaps.model == model706)
    {
        m.filter = '\x01';
    }
    if (m.mk == modeWFM)
    {
        m.filter = '\x01';
    }

    if(m.VFO==inactiveVFO)
    {
        payload.setRawData("\x26\x01", 2);
    } else {
        payload.setRawData("\x06", 1);
    }

    payload.append(m.reg);
    if(m.VFO==inactiveVFO)
        payload.append("\x00", 1);

    payload.append(m.filter);

    prepDataAndSend(payload);
}

void rigCommander::setMode(unsigned char mode, unsigned char modeFilter)
{
    QByteArray payload;
    if(mode < 0x22 + 1)
    {
        // mode command | filter
        // 0x01 | Filter 01 automatically
        // 0x04 | user-specififed 01, 02, 03 | note, is "read the current mode" on older rigs
        // 0x06 | "default" filter is auto

        payload.setRawData("\x06", 1); // cmd 06 needs filter specified
        //payload.setRawData("\x04", 1); // cmd 04 will apply the default filter, but it seems to always pick FIL 02

        payload.append(mode);
        if(rigCaps.model==model706)
        {
            payload.append("\x01"); // "normal" on IC-706
        } else {
            if(mode == 0x06)
            {
                payload.append(0x01);
            } else {
                payload.append(modeFilter);
            }
        }

        prepDataAndSend(payload);
    }
}

void rigCommander::setDataMode(bool dataOn, unsigned char filter)
{
    QByteArray payload;

    payload.setRawData("\x1A\x06", 2);
    if(dataOn)
    {
        payload.append("\x01", 1); // data mode on
        payload.append(filter);

    } else {
        payload.append("\x00\x00", 2); // data mode off, bandwidth not defined per ICD.
    }
    prepDataAndSend(payload);
}

void rigCommander::getFrequency(unsigned char vfo)
{
    if (rigCaps.hasVFOAB || rigCaps.hasVFOMS)
    {
        QByteArray payload("\x25");
        payload.append(vfo);
        prepDataAndSend(payload);
    } else {
        getFrequency();
    }
}

void rigCommander::getFrequency()
{
    // figure out frequency and then respond with haveFrequency();
    // send request to radio
    // 1. make the data
    QByteArray payload("\x03");
    prepDataAndSend(payload);
}

void rigCommander::getMode()
{
    QByteArray payload("\x04");
    prepDataAndSend(payload);
}

void rigCommander::getDataMode()
{
    QByteArray payload("\x1A\x06");
    prepDataAndSend(payload);
}

void rigCommander::getSplit()
{
    QByteArray payload("\x0F");
    prepDataAndSend(payload);
}

void rigCommander::setSplit(bool splitEnabled)
{
    QByteArray payload("\x0F");
    payload.append((unsigned char)splitEnabled);
    prepDataAndSend(payload);
}

void rigCommander::setDuplexMode(duplexMode dm)
{
    QByteArray payload;
    if(dm==dmDupAutoOff)
    {
        payload.setRawData("\x1A\x05\x00\x46\x00", 5);
    } else if (dm==dmDupAutoOn)
    {
        payload.setRawData("\x1A\x05\x00\x46\x01", 5);
    } else {
        payload.setRawData("\x0F", 1);
        payload.append((unsigned char) dm);
    }
    prepDataAndSend(payload);
}

void rigCommander::getDuplexMode()
{
    QByteArray payload;

    // Duplex mode:
    payload.setRawData("\x0F", 1);
    prepDataAndSend(payload);

    // Auto Repeater Mode:
    payload.setRawData("\x1A\x05\x00\x46", 4);
    prepDataAndSend(payload);
}

void rigCommander::setQuickSplit(bool qsOn)
{
    if(rigCaps.hasQuickSplitCommand)
    {
        QByteArray payload = rigCaps.quickSplitCommand;
        payload.append((unsigned char)qsOn);
        prepDataAndSend(payload);
    }
}

void rigCommander::setPassband(quint16 pass)
{
    QByteArray payload;
    payload.setRawData("\x1A\x03", 2);

    /*
    * Passband is calculated as follows, if a higher than legal value is provided,
    * It will set passband to the maximum allowed for the particular mode.
    * 
    * Mode                Data        Steps
    * SSB/CW/RTTY/PSK     0 to 9      50 ~ 500 Hz (50 Hz)
    * SSB/CW/PSK          10 to 40    600 Hz ~ 3.6 kHz (100 Hz)
    * RTTY                10 to 31    600 ~ 2.7 kHz (100 Hz)
    * AM                  0 to 49     200 Hz ~ 10.0 kHz (200 Hz)
    */
    
    unsigned char calc;
    if (state.getChar(MODE) == modeAM) { // AM 0-49

        calc = quint16((pass / 200) - 1);
        if (calc > 49)
            calc = 49;
    }
    else if (pass >= 600) // SSB/CW/PSK 10-40 (10-31 for RTTY)
    {
        calc = quint16((pass / 100) + 4);
        if (((calc > 31) && (state.getChar(MODE) == modeRTTY || state.getChar(MODE) == modeRTTY_R)))
        {
            calc = 31;
        } 
        else if (calc > 40) {
            calc = 40;
        }
    }
    else {  // SSB etc 0-9
        calc = quint16((pass / 50) - 1);
    }
    
    char tens = (calc / 10);
    char units = (calc - (10 * tens));

    char b1 = (units) | (tens << 4);

    payload.append(b1);
    prepDataAndSend(payload);
}

void rigCommander::getPassband()
{
    QByteArray payload;
    payload.setRawData("\x1A\x03", 2);
    prepDataAndSend(payload);
}

void rigCommander::getCwPitch()
{
    QByteArray payload;
    payload.setRawData("\x14\x09", 2);
    prepDataAndSend(payload);
}

void rigCommander::setCwPitch(unsigned char pitch)
{
    QByteArray payload;
    payload.setRawData("\x14\x09", 2);
    payload.append(bcdEncodeInt(pitch));
    prepDataAndSend(payload);
}

void rigCommander::getDashRatio()
{
    QByteArray payload;
    switch (rigCaps.model)
    {
    case model705:
        payload.setRawData("\x1A\x05\x02\x52", 4);
        break;
    case model9700:
        payload.setRawData("\x1A\x05\x02\x24", 4);
        break;
    case model7100:
        payload.setRawData("\x1A\x05\x01\x35", 4);
        break;
    case model7300:
        payload.setRawData("\x1A\x05\x01\x61", 4);
        break;
    case model7610:
        payload.setRawData("\x1A\x05\x02\x28", 4);
        break;
    case model7700:
        payload.setRawData("\x1A\x05\x01\x34", 4);
        break;
    case model7850:
        payload.setRawData("\x1A\x05\x02\x51", 4);
        break;
    default:
        break;
    }
    prepDataAndSend(payload);
}

void rigCommander::setDashRatio(unsigned char ratio)
{
    QByteArray payload;
    switch (rigCaps.model)
    {
    case model705:
        payload.setRawData("\x1A\x05\x02\x52", 4);
        break;
    case model9700:
        payload.setRawData("\x1A\x05\x02\x24", 4);
        break;
    case model7100:
        payload.setRawData("\x1A\x05\x01\x35", 4);
        break;
    case model7300:
        payload.setRawData("\x1A\x05\x01\x61", 4);
        break;
    case model7610:
        payload.setRawData("\x1A\x05\x02\x28", 4);
        break;
    case model7700:
        payload.setRawData("\x1A\x05\x01\x34", 4);
        break;
    case model7850:
        payload.setRawData("\x1A\x05\x02\x51", 4);
        break;
    default:
        break;
    }

    payload.append(bcdEncodeInt(ratio).at(1)); // Discard first byte
    prepDataAndSend(payload);
}

void rigCommander::getPskTone()
{
    QByteArray payload;
    payload.setRawData("\x1a\x05\x00\x44", 4);
    prepDataAndSend(payload);
}

void rigCommander::setPskTone(unsigned char tone)
{
    QByteArray payload;
    payload.setRawData("\x1a\x05\x00\x44", 4);
    payload.append(bcdEncodeInt(tone));
    prepDataAndSend(payload);
}

void rigCommander::getRttyMark()
{
    QByteArray payload;
    payload.setRawData("\x1a\x05\x00\x41", 4);
    prepDataAndSend(payload);
}

void rigCommander::setRttyMark(unsigned char mark)
{
    QByteArray payload;
    payload.setRawData("\x1a\x05\x00\x41", 4);
    payload.append(bcdEncodeInt(mark));
    prepDataAndSend(payload);
}

void rigCommander::getTransmitFrequency()
{
    QByteArray payload;
    payload.setRawData("\x1C\x03", 2);
    prepDataAndSend(payload);
}

void rigCommander::setTone(quint16 tone)
{
    rptrTone_t t;
    t.tone = tone;
    setTone(t);
}

void rigCommander::setTone(rptrTone_t t)
{
    quint16 tone = t.tone;

    QByteArray fenc = encodeTone(tone);

    QByteArray payload;
    payload.setRawData("\x1B\x00", 2);
    payload.append(fenc);

    if(t.useSecondaryVFO)
    {
        qDebug(logRig()) << "Sending TONE to secondary VFO";
        payload.prepend("\x29\x01");
        //printHex(payload);
    }

    prepDataAndSend(payload);
}

void rigCommander::setTSQL(quint16 t)
{
    rptrTone_t tn;
    tn.tone = t;
    setTSQL(tn);
}

void rigCommander::setTSQL(rptrTone_t t)
{
    quint16 tsql = t.tone;

    QByteArray fenc = encodeTone(tsql);

    QByteArray payload;
    payload.setRawData("\x1B\x01", 2);
    payload.append(fenc);

    if(t.useSecondaryVFO)
    {
        qDebug(logRig()) << "Sending TSQL to secondary VFO";
        payload.prepend("\x29\x01");
        //printHex(payload);
    }

    prepDataAndSend(payload);
}

void rigCommander::setDTCS(quint16 dcscode, bool tinv, bool rinv)
{

    QByteArray denc = encodeTone(dcscode, tinv, rinv);

    QByteArray payload;
    payload.setRawData("\x1B\x02", 2);
    payload.append(denc);

    //qInfo() << __func__ << "DTCS encoded payload: ";
    //printHex(payload);

    prepDataAndSend(payload);
}

QByteArray rigCommander::encodeTone(quint16 tone)
{
    return encodeTone(tone, false, false);
}

QByteArray rigCommander::encodeTone(quint16 tone, bool tinv, bool rinv)
{
    // This function is fine to use for DTCS and TONE
    QByteArray enct;

    unsigned char inv=0;
    inv = inv | (unsigned char)rinv;
    inv = inv | ((unsigned char)tinv) << 4;

    enct.append(inv);

    unsigned char hundreds = tone / 1000;
    unsigned char tens = (tone-(hundreds*1000)) / 100;
    unsigned char ones = (tone -(hundreds*1000)-(tens*100)) / 10;
    unsigned char dec =  (tone -(hundreds*1000)-(tens*100)-(ones*10));

    enct.append(tens | (hundreds<<4));
    enct.append(dec | (ones <<4));

    return enct;
}

quint16 rigCommander::decodeTone(QByteArray eTone)
{
    bool t;
    bool r;
    return decodeTone(eTone, t, r);
}

quint16 rigCommander::decodeTone(QByteArray eTone, bool &tinv, bool &rinv)
{
    // index:  00 01  02 03 04
    // CTCSS:  1B 01  00 12 73 = PL 127.3, decode as 1273
    // D(T)CS: 1B 01  TR 01 23 = T/R Invert bits + DCS code 123

    if (eTone.length() < 5) {
        return 0;
    }
    tinv = false; rinv = false;
    quint16 result = 0;

    if((eTone.at(2) & 0x01) == 0x01)
        tinv = true;
    if((eTone.at(2) & 0x10) == 0x10)
        rinv = true;

    result += (eTone.at(4) & 0x0f);
    result += ((eTone.at(4) & 0xf0) >> 4) *   10;
    result += (eTone.at(3) & 0x0f) *  100;
    result += ((eTone.at(3) & 0xf0) >> 4) * 1000;

    return result;
}

void rigCommander::getTone()
{
    QByteArray payload;
    payload.setRawData("\x1B\x00", 2);
    prepDataAndSend(payload);
}

void rigCommander::getTSQL()
{
    QByteArray payload;
    payload.setRawData("\x1B\x01", 2);
    prepDataAndSend(payload);
}

void rigCommander::getDTCS()
{
    QByteArray payload;
    payload.setRawData("\x1B\x02", 2);
    prepDataAndSend(payload);
}

void rigCommander::getRptAccessMode()
{
    QByteArray payload;
    payload.setRawData("\x16\x5D", 2);
    prepDataAndSend(payload);
}

void rigCommander::setRptAccessMode(rptAccessTxRx ratr)
{
    rptrAccessData_t rd;
    rd.accessMode = ratr;
    setRptAccessMode(rd);
}

void rigCommander::setRptAccessMode(rptrAccessData_t rd)
{
    // NB: This function is the only recommended
    // function to be used for toggling tone and tone squelch.

    QByteArray payload;
    if(rigCaps.hasAdvancedRptrToneCmds)
    {
        // IC-9700 basically
        payload.setRawData("\x16\x5D", 2);
        payload.append((unsigned char)rd.accessMode);
    } else {
        // These radios either don't support DCS or
        // we just haven't added DCS yet.

        // 16 42 00 = TONE off
        // 16 42 01 = TONE on
        // 16 43 00 = TSQL off
        // 16 43 01 = TSQL on

        switch(rd.accessMode)
        {
        case ratrNN:
            // No tone at all
            if(rd.turnOffTone)
            {
                payload.append("\x16\x42\x00", 3); // TONE off
            } else if (rd.turnOffTSQL)
            {
                payload.append("\x16\x43\x00", 3);  // TSQL off
            }
            break;
        case ratrTN:
            // TONE on transmit only
            payload.append("\x16\x42\x01", 3); // TONE on
            break;
        case ratrTT:
            // Tone on transmit and TSQL
            payload.append("\x16\x43\x01", 3); // TSQL on
            break;
        case ratrNT:
            // Tone squelch and no tone transmit:
            payload.append("\x16\x43\x01", 3); // TSQL on, close enough here.
            // payload.append("\x16\x42\x00", 3); // TONE off
            break;
        default:
            qWarning(logRig()) << "Cannot set tone mode" << (unsigned char)rd.accessMode << "on rig model" << rigCaps.modelName;
            return;
        }
    }

    if(rd.useSecondaryVFO && rigCaps.hasSpecifyMainSubCmd)
    {
        payload.prepend("\x29\x01");
    }
    prepDataAndSend(payload);
}

void rigCommander::setRptDuplexOffset(freqt f)
{
    QByteArray payload;
    payload.setRawData("\x0D", 1);
    // get f, chop to 10 MHz
    QByteArray freqPayload = makeFreqPayload(f);
    payload.append(freqPayload.mid(1, 3));
    //qInfo(logRig()) << "Here is potential repeater offset setting, not sending to radio:";
    //printHexNow(payload, logRig());
    //QString g = getHex(payload);
    //qInfo(logRig()).noquote().nospace() << g;
    prepDataAndSend(payload);
}

void rigCommander::getRptDuplexOffset()
{
    QByteArray payload;
    payload.setRawData("\x0C", 1);
    prepDataAndSend(payload);
}

void rigCommander::setIPP(bool enabled)
{
    QByteArray payload;
    payload.setRawData("\x16\x65", 2);
    if(enabled)
    {
        payload.append("\x01");
    } else {
        payload.append("\x00");
    }
    prepDataAndSend(payload);
}

void rigCommander::getIPP()
{
    QByteArray payload;
    payload.setRawData("\x16\x65", 2);
    prepDataAndSend(payload);
}

void rigCommander::setSatelliteMode(bool enabled)
{
    QByteArray payload;
    payload.setRawData("\x16\x5A", 2);
    if(enabled)
    {
        payload.append("\x01");
    } else {
        payload.append("\x00");
    }
    prepDataAndSend(payload);
}

void rigCommander::getSatelliteMode()
{
    QByteArray payload;
    payload.setRawData("\x16\x5A", 2);
    prepDataAndSend(payload);
}

void rigCommander::getPTT()
{
    //if(rigCaps.useRTSforPTT && !usingNativeLAN)
    //{
    //    emit havePTTStatus(comm->rtsStatus());
    //} else {
    QByteArray payload;
    payload.setRawData("\x1C\x00", 2);
    prepDataAndSend(payload);
    //}
}

void rigCommander::getBandStackReg(char band, char regCode)
{
    QByteArray payload("\x1A\x01");
    payload.append(band); // [01 through 11]
    payload.append(regCode); // [01...03]. 01 = latest, 03 = oldest
    prepDataAndSend(payload);
}

void rigCommander::setPTT(bool pttOn)
{
    //bool pttAllowed = false;

    if(pttAllowed)
    {
        QByteArray payload("\x1C\x00", 2);
        payload.append((char)pttOn);
        prepDataAndSend(payload);
    }
}

void rigCommander::sendCW(QString textToSend)
{
    if(textToSend.length() >30)
    {
        qCritical(logRig()).nospace() << "Cannot send CW message, length > 30 characters (" << textToSend.length() << ")";
        return;
    }

    QByteArray textData = textToSend.toLocal8Bit();
    unsigned char p=0;
    bool printout=false;
    for(int c=0; c < textData.length(); c++)
    {
        p = textData.at(c);
        if( ( (p >= 0x30) && (p <= 0x39) ) ||
            ( (p >= 0x41) && (p <= 0x5A) ) ||
            ( (p >= 0x61) && (p <= 0x7A) ) ||
            (p==0x2F) || (p==0x3F) || (p==0x2E) ||
            (p==0x2D) || (p==0x2C) || (p==0x3A) ||
            (p==0x27) || (p==0x28) || (p==0x29) ||
            (p==0x3D) || (p==0x2B) || (p==0x22) ||
            (p==0x40) || (p==0x20) )
        {
            // Allowed character, continue
        } else {
            qWarning(logRig()) << "Invalid character detected in CW message at position " << c << ", the character is " << textToSend.at(c);
            printout = true;
            textData[c] = 0x3F; // "?"
        }
    }
    if(printout)
        printHex(textData);

    if(pttAllowed)
    {
        QByteArray payload("\x17", 1);
        payload.append(textData);
        prepDataAndSend(payload);
    }
    // Does it need to end in "FF" or is that implicit at the end of a message?
}

void rigCommander::sendStopCW()
{
    QByteArray payload("\x17", 1);
    payload.append("\xFF");
    prepDataAndSend(payload);
}

void rigCommander::setCIVAddr(unsigned char civAddr)
{
    // Note: This sets the radio's CIV address
    // the computer's CIV address is defined in the header file.

    this->civAddr = civAddr;
    payloadPrefix = QByteArray("\xFE\xFE");
    payloadPrefix.append(civAddr);
    payloadPrefix.append((char)compCivAddr);
}

void rigCommander::handleNewData(const QByteArray& data)
{
    emit haveDataForServer(data);
    parseData(data);
}

void rigCommander::receiveAudioData(const audioPacket& data)
{
    emit haveAudioData(data);
}

void rigCommander::parseData(QByteArray dataInput)
{
    // TODO: Clean this up.
    // It finally works very nicely, needs to be streamlined.
    //

    int index = 0;
    volatile int count = 0; // debug purposes

    // use this:
    QList <QByteArray> dataList = dataInput.split('\xFD');
    QByteArray data;
    // qInfo(logRig()) << "data list has this many elements: " << dataList.size();
    if (dataList.last().isEmpty())
    {
        dataList.removeLast(); // if the original ended in FD, then there is a blank entry at the end.
    }
    // Only thing is, each frame is missing '\xFD' at the end. So append! Keeps the frames intact.
    for(index = 0; index < dataList.count(); index++)
    {
        data = dataList[index];
        data.append('\xFD'); // because we expect it to be there.
    // foreach(listitem)
    // listitem.append('\xFD');
    // continue parsing...

        count++;
        // Data echo'd back from the rig start with this:
        // fe fe 94 e0 ...... fd

        // Data from the rig that is not an echo start with this:
        // fe fe e0 94 ...... fd (for example, a reply to a query)

        // Data from the rig that was not asked for is sent to controller 0x00:
        // fe fe 00 94 ...... fd (for example, user rotates the tune control or changes the mode)

        //qInfo(logRig()) << "Data received: ";
        //printHex(data, false, true);
        if(data.length() < 4)
        {
            if(data.length())
            {
                // Finally this almost never happens
                // qInfo(logRig()) << "Data length too short: " << data.length() << " bytes. Data:";
                //printHex(data, false, true);
            }
            // no
            //return;
            // maybe:
            // continue;
        }

        if(!data.startsWith("\xFE\xFE"))
        {
            // qInfo(logRig()) << "Warning: Invalid data received, did not start with FE FE.";
            // find 94 e0 and shift over,
            // or look inside for a second FE FE
            // Often a local echo will miss a few bytes at the beginning.
            if(data.startsWith('\xFE'))
            {
                data.prepend('\xFE');
                // qInfo(logRig()) << "Warning: Working with prepended data stream.";
                parseData(payloadIn);
                return;
            } else {
                //qInfo(logRig()) << "Error: Could not reconstruct corrupted data: ";
                //printHex(data, false, true);
                // data.right(data.length() - data.find('\xFE\xFE'));
                // if found do not return and keep going.
                return;
            }
        }

        if((unsigned char)data[02] == civAddr)
        {
            // data is or begins with an echoback from what we sent
            // find the first 'fd' and cut it. Then continue.
            //payloadIn = data.right(data.length() - data.indexOf('\xfd')-1);
            // qInfo(logRig()) << "[FOUND] Trimmed off echo:";
            //printHex(payloadIn, false, true);
            //parseData(payloadIn);
            //return;
        }

        incomingCIVAddr = data[03]; // track the CIV of the sender.

        switch(data[02])
        {
            //    case civAddr: // can't have a variable here :-(
            //        // data is or begins with an echoback from what we sent
            //        // find the first 'fd' and cut it. Then continue.
            //        payloadIn = data.right(data.length() - data.indexOf('\xfd')-1);
            //        //qInfo(logRig()) << "Trimmed off echo:";
            //        //printHex(payloadIn, false, true);
            //        parseData(payloadIn);
            //        break;
            // case '\xE0':

            case (char)0xE0:
            case (char)compCivAddr:
                // data is a reply to some query we sent
                // extract the payload out and parse.
                // payload = getpayload(data); // or something
                // parse (payload); // recursive ok?
                payloadIn = data.right(data.length() - 4);
                if(payloadIn.contains("\xFE"))
                {
                    //qDebug(logRig()) << "Corrupted data contains FE within message body: ";
                    //printHex(payloadIn);
                    break;
                }
                parseCommand();
                break;
            case '\x00':
                // data send initiated by the rig due to user control
                // extract the payload out and parse.
                if((unsigned char)data[03]==compCivAddr)
                {
                    // This is an echo of our own broadcast request.
                    // The data are "to 00" and "from E1"
                    // Don't use it!
                    qDebug(logRig()) << "Caught it! Found the echo'd broadcast request from us! Rig has not responded to broadcast query yet.";
                } else {
                    payloadIn = data.right(data.length() - 4); // Removes FE FE E0 94 part
                    if(payloadIn.contains("\xFE"))
                    {
                        //qDebug(logRig()) << "Corrupted data contains FE within message body: ";
                        //printHex(payloadIn);
                        break;
                    }
                    parseCommand();
                }
                break;
            default:
                // could be for other equipment on the CIV network.
                // just drop for now.
                // relaySendOutData(data);
                break;
        }
    }
    /*
    if(dataList.length() > 1)
    {
        qInfo(logRig()) << "Recovered " << count << " frames from single data with size" << dataList.count();
    }
    */
}

void rigCommander::parseCommand()
{
    // note: data already is trimmed of the beginning FE FE E0 94 stuff.

    bool isSpectrumData = payloadIn.startsWith(QByteArray().setRawData("\x27\x00", 2));

    if( (!isSpectrumData) && (payloadIn[00] != '\x15') )
    {
        // We do not log spectrum and meter data,
        // as they tend to clog up any useful logging.
        qDebug(logRigTraffic()) << "Received from radio:";
        printHexNow(payloadIn, logRigTraffic());
    }

    switch(payloadIn[00])
    {

        case 00:
            // frequency data
            parseFrequency();
            break;
        case 03:
            parseFrequency();
            break;
        case '\x25':
            // Parse both VFOs
            emit haveFrequency(parseFrequency(payloadIn, 5));
        break;
        case '\x01':
            //qInfo(logRig()) << "Have mode data";
            this->parseMode();
            break;
        case '\x04':
            //qInfo(logRig()) << "Have mode data";
            this->parseMode();
            break;
        case '\x05':
            //qInfo(logRig()) << "Have frequency data";
            this->parseFrequency();
            break;
        case '\x06':
            //qInfo(logRig()) << "Have mode data";
            this->parseMode();
            break;
        case '\x0C':
            //qDebug(logRig) << "Have 0x0C reply";
            emit haveRptOffsetFrequency(parseFrequencyRptOffset(payloadIn));
            break;
        case '\x0F':
            emit haveDuplexMode((duplexMode)(unsigned char)payloadIn[1]);
            state.set(DUPLEX, (duplexMode)(unsigned char)payloadIn[1], false);
            break;
        case '\x11':
            emit haveAttenuator((unsigned char)payloadIn.at(1));
            state.set(ATTENUATOR, (quint8)payloadIn[1], false);
            break;
        case '\x12':
            emit haveAntenna((unsigned char)payloadIn.at(1), (bool)payloadIn.at(2));
            state.set(ANTENNA, (quint8)payloadIn[1], false);
            state.set(RXANTENNA, (bool)payloadIn[2], false);
            break;
        case '\x14':
            // read levels
            parseLevels();
            break;
        case '\x15':
            // Metering such as s, power, etc
            parseLevels();
            break;
        case '\x16':
            parseRegister16();
            break;
        case '\x19':
            // qInfo(logRig()) << "Have rig ID: " << (unsigned int)payloadIn[2];
            // printHex(payloadIn, false, true);
            model = determineRadioModel(payloadIn[2]); // verify this is the model not the CIV
            rigCaps.modelID = payloadIn[2];
            determineRigCaps();
            qInfo(logRig()) << "Have rig ID: decimal: " << (unsigned int)rigCaps.modelID;


            break;
        case '\x21':
            // RIT and Delta TX:
            parseRegister21();
            break;
        case '\x26':
            if((int)payloadIn[1] == 0)
            {
                // This works but LSB comes out as CW?
                // Also, an opportunity to read the data mode
                // payloadIn = payloadIn.right(3);
                // this->parseMode();
            }
            break;
        case '\x27':
            // scope data
            //qInfo(logRig()) << "Have scope data";
            //printHex(payloadIn, false, true);
            parseWFData();
            //parseSpectrum();
            break;
        case '\x1A':
            if(payloadIn[01] == '\x05')
            {
                parseDetailedRegisters1A05();
            } else {
                parseRegisters1A();
            }
            break;
        case '\x1B':
            parseRegister1B();
            break;
        case '\x1C':
            parseRegisters1C();
            break;
        case '\xFB':
            // Fine Business, ACK from rig.
            break;
        case '\xFA':
            // error

            qDebug(logRig()) << "Error (FA) received from rig.";
            printHex(payloadIn, false ,true);
            break;

        default:
            // This gets hit a lot when the pseudo-term is
            // using commands wfview doesn't know yet.
            // qInfo(logRig()) << "Have other data with cmd: " << std::hex << payloadIn[00];
            // printHex(payloadIn, false, true);
            break;
    }
    // is any payload left?

}

void rigCommander::parseLevels()
{
    //qInfo(logRig()) << "Received a level status readout: ";
    // printHex(payloadIn, false, true);

    // wrong: unsigned char level = (payloadIn[2] * 100) + payloadIn[03];
    unsigned char hundreds = payloadIn[2];
    unsigned char tens = (payloadIn[3] & 0xf0) >> 4;
    unsigned char units = (payloadIn[3] & 0x0f);

    unsigned char level = ((unsigned char)100*hundreds) + (10*tens) + units;

    //qInfo(logRig()) << "Level is: " << (int)level << " or " << 100.0*level/255.0 << "%";

    // Typical RF gain response (rather low setting):
    // "INDEX: 00 01 02 03 04 "
    // "DATA:  14 02 00 78 fd "

    if(payloadIn[0] == '\x14')
    {
        switch(payloadIn[1])
        {
            case '\x01':
                // AF level - ignore if LAN connection.
                if (udp == Q_NULLPTR) {
                    emit haveAfGain(level);
                    state.set(AFGAIN, level, false);
                }
                else {
                    state.set(AFGAIN, localVolume, false);
                }
                break;
            case '\x02':
                // RX RF Gain
                emit haveRfGain(level);
                state.set(RFGAIN, level, false);
                break;
            case '\x03':
                // Squelch level
                emit haveSql(level);                
                state.set(SQUELCH, level, false);
                break;
            case '\x07':
                // Twin BPF Inner, or, IF-Shift level
                if(rigCaps.hasTBPF)
                    emit haveTPBFInner(level);
                else
                    emit haveIFShift(level);
                state.set(PBTIN, level, false);
                break;
            case '\x08':
                // Twin BPF Outer
                emit haveTPBFOuter(level);
                state.set(PBTOUT, level, false);
                break;
            case '\x06':
                // NR Level
                emit haveNRLevel(level);
                state.set(NR, level, false);
                break;
            case '\x09':
                // CW Pitch
                emit haveCwPitch(level);
                state.set(CWPITCH, level, false);
                break;
            case '\x0A':
                // TX RF level
                emit haveTxPower(level);
                state.set(RFPOWER, level, false);
                break;
            case '\x0B':
                // Mic Gain
                emit haveMicGain(level);
                state.set(MICGAIN, level, false);
                break;
            case '\x0C':
                state.set(KEYSPD, level, false);
                //qInfo(logRig()) << "Have received key speed in RC, raw level: " << level << ", WPM: " << (level/6.071)+6 << ", rounded: " << round((level/6.071)+6);
                emit haveKeySpeed(round((level / 6.071) + 6));
                break;
            case '\x0D':
                // Notch filder setting - ignore for now
                state.set(NOTCHF, level, false);
                break;
            case '\x0E':
                // compressor level
                emit haveCompLevel(level);
                state.set(COMPLEVEL, level, false);
                break;
            case '\x12':
                emit haveNB((bool)level);
                state.set(NB, level, false);
                break;
            case '\x15':
                // monitor level
                emit haveMonitorGain(level);
                state.set(MONITORLEVEL, level, false);
                break;
            case '\x16':
                // VOX gain
                emit haveVoxGain(level);
                state.set(VOXGAIN, level, false);
                break;
            case '\x17':
                // anti-VOX gain
                emit haveAntiVoxGain(level);
                state.set(ANTIVOXGAIN, level, false);
                break;

            default:
                qInfo(logRig()) << "Unknown control level (0x14) received at register " << QString("0x%1").arg((int)payloadIn[1],2,16) << " with level " << QString("0x%1").arg((int)level,2,16) << ", int=" << (int)level;
                printHex(payloadIn);
                break;
        }
        return;
    }

    if(payloadIn[0] == '\x15')
    {
        switch(payloadIn[1])
        {
            case '\x01':
                // noise or s-meter sequelch status
                break;
            case '\x02':
                // S-Meter
                emit haveMeter(meterS, level);
                state.set(SMETER, level, false);
                break;
            case '\x04':
                // Center (IC-R8600)
                emit haveMeter(meterCenter, level);
                state.set(SMETER, level, false);
                break;
            case '\x05':
                // Various squelch (tone etc.)
                break;
            case '\x11':
                // RF-Power meter
                emit haveMeter(meterPower, level);
                state.set(POWERMETER, level, false);
                break;
            case '\x12':
                // SWR
                emit haveMeter(meterSWR, level);
                state.set(SWRMETER, level, false);
                break;
            case '\x13':
                // ALC
                emit haveMeter(meterALC, level);
                state.set(ALCMETER, level, false);
                break;
            case '\x14':
                // COMP dB reduction
                emit haveMeter(meterComp, level);
                state.set(COMPMETER, level, false);
                break;
            case '\x15':
                // VD (12V)
                emit haveMeter(meterVoltage, level);
                state.set(VOLTAGEMETER, level, false);
                break;
            case '\x16':
                // ID
                emit haveMeter(meterCurrent, level);
                state.set(CURRENTMETER, level, false);
                break;

            default:
                qInfo(logRig()) << "Unknown meter level (0x15) received at register " << (unsigned int) payloadIn[1] << " with level " << level;
                break;
        }


    return;
    }

}

void rigCommander::setIFShift(unsigned char level)
{
    QByteArray payload("\x14\x07");
    payload.append(bcdEncodeInt(level));
    prepDataAndSend(payload);
}

void rigCommander::setTPBFInner(unsigned char level)
{
    QByteArray payload("\x14\x07");
    payload.append(bcdEncodeInt(level));
    prepDataAndSend(payload);
}

void rigCommander::setTPBFOuter(unsigned char level)
{
    QByteArray payload("\x14\x08");
    payload.append(bcdEncodeInt(level));
    prepDataAndSend(payload);
}

void rigCommander::setTxPower(unsigned char power)
{
    QByteArray payload("\x14\x0A");
    payload.append(bcdEncodeInt(power));
    prepDataAndSend(payload);
}

void rigCommander::setMicGain(unsigned char gain)
{
    QByteArray payload("\x14\x0B");
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

void rigCommander::getModInput(bool dataOn)
{
    setModInput(inputMic, dataOn, true);
}

void rigCommander::setModInput(rigInput input, bool dataOn)
{
    setModInput(input, dataOn, false);
}

void rigCommander::setModInput(rigInput input, bool dataOn, bool isQuery)
{
//    The input enum is as follows:

//    inputMic=0,
//    inputACC=1,
//    inputUSB=3,
//    inputLAN=5,
//    inputACCA,
//    inputACCB};

    QByteArray payload;
    QByteArray inAsByte;

    if(isQuery)
        input = inputMic;


    switch(rigCaps.model)
    {
        case model9700:
            payload.setRawData("\x1A\x05\x01\x15", 4);
            payload.append((unsigned char)input);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x00\x91", 4);
            payload.append((unsigned char)input);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x66", 4);
            payload.append((unsigned char)input);
            break;
        case model7850:
            payload.setRawData("\x1A\x05\x00\x63", 4);
            switch(input)
            {
                case inputMic:
                    inAsByte.setRawData("\x00", 1);
                    break;
                case inputACCA:
                    inAsByte.setRawData("\x01", 1);
                    break;
                case inputACCB:
                    inAsByte.setRawData("\x02", 1);
                    break;
                case inputUSB:
                    inAsByte.setRawData("\x08", 1);
                    break;
                case inputLAN:
                    inAsByte.setRawData("\x09", 1);
                    break;
                default:
                    return;

            }
            payload.append(inAsByte);
            break;
        case model705:
            payload.setRawData("\x1A\x05\x01\x18", 4);
            switch(input)
            {
                case inputMic:
                    inAsByte.setRawData("\x00", 1);
                    break;
                case inputUSB:
                    inAsByte.setRawData("\x01", 1);
                    break;
                case inputLAN: // WLAN
                    inAsByte.setRawData("\x03", 1);
                    break;
                default:
                    return;
            }
            payload.append(inAsByte);
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x00\x32", 4);
            if(input==inputLAN)
            {
                // NOTE: CIV manual says data may range from 0 to 3
                // But data 0x04 does correspond to LAN.
                payload.append("\x04");
            } else {
                payload.append((unsigned char)input);
            }
            break;
        case model7600:
            payload.setRawData("\x1A\x05\x00\x30", 4);
            payload.append((unsigned char)input);
            break;
        case model7100:
            payload.setRawData("\x1A\x05\x00\x90", 4);
            payload.append((unsigned char)input);
            break;
        case model7200:
            payload.setRawData("\x1A\x03\x23", 3);
            switch(input)
            {
                case inputMic:
                    payload.setRawData("\x00", 1);
                    break;
                case inputUSB:
                    payload.setRawData("\x03", 1);
                    break;
                case inputACC:
                    payload.setRawData("\x01", 1);
                    break;
                default:
                    return;
            }
        default:
            break;
    }
    if(dataOn)
    {
        if(rigCaps.model==model7200)
        {
            payload[2] = payload[2] + 1;
        } else {
            payload[3] = payload[3] + 1;
        }
    }

    if(isQuery)
    {
        payload.truncate(4);
    }

    prepDataAndSend(payload);

}

void rigCommander::setModInputLevel(rigInput input, unsigned char level)
{
    switch(input)
    {
        case inputMic:
            setMicGain(level);
            break;

        case inputACCA:
            setACCGain(level, 0);
            break;

        case inputACCB:
            setACCGain(level, 1);
            break;

        case inputACC:
            setACCGain(level);
            break;

        case inputUSB:
            setUSBGain(level);
            break;

        case inputLAN:
            setLANGain(level);
            break;

        default:
            break;
    }
}

void rigCommander::setAfMute(bool gainOn)
{
    QByteArray payload("\x1a\x09");
    payload.append((quint8)gainOn);
    prepDataAndSend(payload);
}

void rigCommander::setDialLock(bool lockOn)
{
    QByteArray payload("\x16\x50");
    payload.append((quint8)lockOn);
    prepDataAndSend(payload);
}

void rigCommander::getModInputLevel(rigInput input)
{
    switch(input)
    {
        case inputMic:
            getMicGain();
            break;

        case inputACCA:
            getACCGain(0);
            break;

        case inputACCB:
            getACCGain(1);
            break;

        case inputACC:
            getACCGain();
            break;

        case inputUSB:
            getUSBGain();
            break;

        case inputLAN:
            getLANGain();
            break;

        default:
            break;
    }
}

void rigCommander::getAfMute()
{
    QByteArray payload("\x1a\x09");
    prepDataAndSend(payload);
}

void rigCommander::getDialLock()
{
    QByteArray payload("\x16\x50");
    prepDataAndSend(payload);
}

QByteArray rigCommander::getUSBAddr()
{
    QByteArray payload;

    switch(rigCaps.model)
    {
        case model705:
            payload.setRawData("\x1A\x05\x01\x16", 4);
            break;
        case model9700:
            payload.setRawData("\x1A\x05\x01\x13", 4);
            break;
        case model7200:
            payload.setRawData("\x1A\x03\x25", 3);
            break;
        case model7100:
        case model7610:
            payload.setRawData("\x1A\x05\x00\x89", 4);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x65", 4);
            break;
        case model7850:
            payload.setRawData("\x1A\x05\x00\x61", 4);
            break;
        case model7600:
            payload.setRawData("\x1A\x05\x00\x29", 4);
            break;
        default:
            break;
    }
    return payload;
}

void rigCommander::getUSBGain()
{
    QByteArray payload = getUSBAddr();
    prepDataAndSend(payload);
}


void rigCommander::setUSBGain(unsigned char gain)
{
    QByteArray payload = getUSBAddr();
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

QByteArray rigCommander::getLANAddr()
{
    QByteArray payload;
    switch(rigCaps.model)
    {
        case model705:
            payload.setRawData("\x1A\x05\x01\x17", 4);
            break;
        case model9700:
            payload.setRawData("\x1A\x05\x01\x14", 4);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x00\x90", 4);
            break;
        case model7850:
            payload.setRawData("\x1A\x05\x00\x62", 4);
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x01\x92", 4);
            break;
        default:
            break;
    }

    return payload;
}

void rigCommander::getLANGain()
{
    QByteArray payload = getLANAddr();
    prepDataAndSend(payload);
}

void rigCommander::setLANGain(unsigned char gain)
{
    QByteArray payload = getLANAddr();
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

QByteArray rigCommander::getACCAddr(unsigned char ab)
{
    QByteArray payload;

    // Note: the manual for the IC-7600 does not call out a
    // register to adjust the ACC gain.

    // 7850: ACC-A = 0, ACC-B = 1

    switch(rigCaps.model)
    {
        case model9700:
            payload.setRawData("\x1A\x05\x01\x12", 4);
            break;
        case model7100:
            payload.setRawData("\x1A\x05\x00\x87", 4);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x00\x88", 4);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x64", 4);
            break;
        case model7850:
            // Note: 0x58 = ACC-A, 0x59 = ACC-B
            if(ab==0)
            {
                // A
                payload.setRawData("\x1A\x05\x00\x58", 4);
            } else {
                // B
                payload.setRawData("\x1A\x05\x00\x59", 4);
            }
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x00\x30", 4);
            break;
        default:
            break;
    }

    return payload;
}

void rigCommander::getACCGain()
{
    QByteArray payload = getACCAddr(0);
    prepDataAndSend(payload);
}


void rigCommander::getACCGain(unsigned char ab)
{
    QByteArray payload = getACCAddr(ab);
    prepDataAndSend(payload);
}


void rigCommander::setACCGain(unsigned char gain)
{
    QByteArray payload = getACCAddr(0);
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

void rigCommander::setACCGain(unsigned char gain, unsigned char ab)
{
    QByteArray payload = getACCAddr(ab);
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

void rigCommander::setCompLevel(unsigned char compLevel)
{
    QByteArray payload("\x14\x0E");
    payload.append(bcdEncodeInt(compLevel));
    prepDataAndSend(payload);
}

void rigCommander::setMonitorGain(unsigned char monitorLevel)
{
    QByteArray payload("\x14\x15");
    payload.append(bcdEncodeInt(monitorLevel));
    prepDataAndSend(payload);
}

void rigCommander::setVoxGain(unsigned char gain)
{
    QByteArray payload("\x14\x16");
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

void rigCommander::setAntiVoxGain(unsigned char gain)
{
    QByteArray payload("\x14\x17");
    payload.append(bcdEncodeInt(gain));
    prepDataAndSend(payload);
}

void rigCommander::setNBLevel(unsigned char level)
{
    if (rigCaps.hasNB) {
        QByteArray payload(rigCaps.nbCommand);
        payload.append(bcdEncodeInt(level));
        prepDataAndSend(payload);
    }
}

void rigCommander::setNRLevel(unsigned char level)
{
    QByteArray payload("\x14\x06");
    payload.append(bcdEncodeInt(level));
    prepDataAndSend(payload);
}


void rigCommander::getRfGain()
{
    QByteArray payload("\x14\x02");
    prepDataAndSend(payload);
}

void rigCommander::getAfGain()
{
    if (udp == Q_NULLPTR) {
        QByteArray payload("\x14\x01");
        prepDataAndSend(payload);
    }
    else {
        emit haveAfGain(localVolume);
    }
}

void rigCommander::getIFShift()
{
    QByteArray payload("\x14\x07");
    prepDataAndSend(payload);
}

void rigCommander::getTPBFInner()
{
    QByteArray payload("\x14\x07");
    prepDataAndSend(payload);
}

void rigCommander::getTPBFOuter()
{
    QByteArray payload("\x14\x08");
    prepDataAndSend(payload);
}

void rigCommander::getSql()
{
    QByteArray payload("\x14\x03");
    prepDataAndSend(payload);
}

void rigCommander::getTxLevel()
{
    QByteArray payload("\x14\x0A");
    prepDataAndSend(payload);
}

void rigCommander::getMicGain()
{
    QByteArray payload("\x14\x0B");
    prepDataAndSend(payload);
}

void rigCommander::getCompLevel()
{
    QByteArray payload("\x14\x0E");
    prepDataAndSend(payload);
}

void rigCommander::getMonitorGain()
{
    QByteArray payload("\x14\x15");
    prepDataAndSend(payload);
}

void rigCommander::getVoxGain()
{
    QByteArray payload("\x14\x16");
    prepDataAndSend(payload);
}

void rigCommander::getAntiVoxGain()
{
    QByteArray payload("\x14\x17");
    prepDataAndSend(payload);
}

void rigCommander::getNBLevel()
{
    if (rigCaps.hasNB) {
        prepDataAndSend(rigCaps.nbCommand);
    }
}

void rigCommander::getNRLevel()
{
    QByteArray payload("\x14\x06");
    prepDataAndSend(payload);
}

void rigCommander::getLevels()
{
    // Function to grab all levels
    getRfGain(); //0x02
    getAfGain(); // 0x01
    getSql(); // 0x03
    getTxLevel(); // 0x0A
    getMicGain(); // 0x0B
    getCompLevel(); // 0x0E
//    getMonitorGain(); // 0x15
//    getVoxGain(); // 0x16
//    getAntiVoxGain(); // 0x17
}

void rigCommander::getMeters(meterKind meter)
{
    switch(meter)
    {
        case meterS:
            getSMeter();
            break;
        case meterCenter:
            getCenterMeter();
            break;
        case meterSWR:
            getSWRMeter();
            break;
        case meterPower:
            getRFPowerMeter();
            break;
        case meterALC:
            getALCMeter();
            break;
        case meterComp:
            getCompReductionMeter();
            break;
        case meterVoltage:
            getVdMeter();
            break;
        case meterCurrent:
            getIDMeter();
            break;
        default:
            break;
    }
}

void rigCommander::getSMeter()
{
    QByteArray payload("\x15\x02");
    prepDataAndSend(payload);
}

void rigCommander::getCenterMeter()
{
    QByteArray payload("\x15\x04");
    prepDataAndSend(payload);
}

void rigCommander::getRFPowerMeter()
{
    QByteArray payload("\x15\x11");
    prepDataAndSend(payload);
}

void rigCommander::getSWRMeter()
{
    QByteArray payload("\x15\x12");
    prepDataAndSend(payload);
}

void rigCommander::getALCMeter()
{
    QByteArray payload("\x15\x13");
    prepDataAndSend(payload);
}

void rigCommander::getCompReductionMeter()
{
    QByteArray payload("\x15\x14");
    prepDataAndSend(payload);
}

void rigCommander::getVdMeter()
{
    QByteArray payload("\x15\x15");
    prepDataAndSend(payload);
}

void rigCommander::getIDMeter()
{
    QByteArray payload("\x15\x16");
    prepDataAndSend(payload);
}


void rigCommander::setSquelch(unsigned char level)
{
    sendLevelCmd(0x03, level);
}

void rigCommander::setRfGain(unsigned char level)
{
    sendLevelCmd(0x02, level);
}

void rigCommander::setAfGain(unsigned char level)
{
    if (udp == Q_NULLPTR) {
        sendLevelCmd(0x01, level);
    }
    else {
        emit haveSetVolume(level);
        localVolume = level;
    }
}

void rigCommander::setRefAdjustCourse(unsigned char level)
{
    // 1A 05 00 72 0000-0255
    QByteArray payload;
    payload.setRawData("\x1A\x05\x00\x72", 4);
    payload.append(bcdEncodeInt((unsigned int)level));
    prepDataAndSend(payload);

}

void rigCommander::setRefAdjustFine(unsigned char level)
{
    qInfo(logRig()) << __FUNCTION__ << " level: " << level;
    // 1A 05 00 73 0000-0255
    QByteArray payload;
    payload.setRawData("\x1A\x05\x00\x73", 4);
    payload.append(bcdEncodeInt((unsigned int)level));
    prepDataAndSend(payload);
}

void rigCommander::setTime(timekind t)
{
    QByteArray payload;

    switch(rigCaps.model)
    {
        case model705:
            payload.setRawData("\x1A\x05\x01\x66", 4);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x95", 4);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x01\x59", 4);
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x00\x59", 4);
            break;
        case model7850:
            payload.setRawData("\x1A\x05\x00\x96", 4);
            break;
        case model9700:
            payload.setRawData("\x1A\x05\x01\x80", 4);
            break;
        case modelR8600:
            payload.setRawData("\x1A\x05\x01\x32", 4);
            break;
        default:
            return;
            break;

    }
    payload.append(convertNumberToHex(t.hours));
    payload.append(convertNumberToHex(t.minutes));
    //qDebug(logRig()) << "Setting time to this: ";
    //printHex(payload);
    prepDataAndSend(payload);
}

void rigCommander::setDate(datekind d)
{
    QByteArray payload;

    switch(rigCaps.model)
    {
        case model705:
            payload.setRawData("\x1A\x05\x01\x65", 4);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x94", 4);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x01\x58", 4);
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x00\x58", 4);
            break;
        case model7850:
            payload.setRawData("\x1A\x05\x00\x95", 4);
            break;
        case model9700:
            payload.setRawData("\x1A\x05\x01\x79", 4);
            break;
        case modelR8600:
            payload.setRawData("\x1A\x05\x01\x31", 4);
            break;
        default:
            return;
            break;

    }
    // YYYYMMDD
    payload.append(convertNumberToHex(d.year/100)); // 20
    payload.append(convertNumberToHex(d.year - 100*(d.year/100))); // 21
    payload.append(convertNumberToHex(d.month));
    payload.append(convertNumberToHex(d.day));
    //qDebug(logRig()) << "Setting date to this: ";
    //printHex(payload);
    prepDataAndSend(payload);
}

void rigCommander::setUTCOffset(timekind t)
{
    QByteArray payload;

    switch(rigCaps.model)
    {
        case model705:
            payload.setRawData("\x1A\x05\x01\x70", 4);
            break;
        case model7300:
            payload.setRawData("\x1A\x05\x00\x96", 4);
            break;
        case model7610:
            payload.setRawData("\x1A\x05\x01\x62", 4);
            break;
        case model7700:
            payload.setRawData("\x1A\x05\x00\x61", 4);
            break;
        case model7850:
            // Clock 1:
            payload.setRawData("\x1A\x05\x00\x99", 4);
            break;
        case model9700:
            payload.setRawData("\x1A\x05\x01\x84", 4);
            break;
        case modelR8600:
            payload.setRawData("\x1A\x05\x01\x35", 4);
            break;
        default:
            return;
            break;

    }
    payload.append(convertNumberToHex(t.hours));
    payload.append(convertNumberToHex(t.minutes));
    payload.append((unsigned char)t.isMinus);
    //qDebug(logRig()) << "Setting UTC Offset to this: ";
    //printHex(payload);
    prepDataAndSend(payload);
}

unsigned char rigCommander::convertNumberToHex(unsigned char num)
{
    // Two digit only
    if(num > 99)
    {
        qInfo(logRig()) << "Invalid numeric conversion from num " << num << " to hex.";
        return 0xFA;
    }
    unsigned char result = 0;
    result =  (num/10) << 4;
    result |= (num - 10*(num/10));
    qDebug(logRig()) << "Converting number: " << num << " to hex: " + QString("0x%1").arg(result, 2, 16, QChar('0'));
    return result;
}

void rigCommander::sendLevelCmd(unsigned char levAddr, unsigned char level)
{
    QByteArray payload("\x14");
    payload.append(levAddr);
    // careful here. The value in the units and tens can't exceed 99.
    // ie, can't do this: 01 f2
    payload.append((int)level/100); // make sure it works with a zero
    // convert the tens:
    int tens = (level - 100*((int)level/100))/10;
    // convert the units:
    int units = level - 100*((int)level/100);
    units = units - 10*((int)(units/10));
    // combine and send:
    payload.append((tens << 4) | (units) ); // make sure it works with a zero

    prepDataAndSend(payload);
}

void rigCommander::getRefAdjustCourse()
{
    // 1A 05 00 72
    QByteArray payload;
    payload.setRawData("\x1A\x05\x00\x72", 4);
    prepDataAndSend(payload);
}

void rigCommander::getRefAdjustFine()
{
    // 1A 05 00 73
    QByteArray payload;
    payload.setRawData("\x1A\x05\x00\x73", 4);
    prepDataAndSend(payload);
}

void rigCommander::parseRegisters1C()
{
    // PTT lives here
    // Not sure if 02 is the right place to switch.
    // TODO: test this function
    switch(payloadIn[01])
    {
        case '\x00':
            parsePTT();
            break;
        case '\x01':
            // ATU status (on/off/tuning)
            parseATU();
            break;
        default:
            break;
    }
}

void rigCommander::parseRegister21()
{
    // Register 21 is RIT and Delta TX
    int ritHz = 0;
    freqt f;
    QByteArray longfreq;

    // Example RIT value reply:
    // Index: 00 01 02 03 04 05
    // DATA:  21 00 32 03 00 fd

    switch(payloadIn[01])
    {
        case '\x00':
            // RIT frequency
            //
            longfreq = payloadIn.mid(2,2);
            longfreq.append(QByteArray(3,'\x00'));
            f = parseFrequency(longfreq, 3);
            if(payloadIn.length() < 5)
                break;
            ritHz = f.Hz*((payloadIn.at(4)=='\x01')?-1:1);
            emit haveRitFrequency(ritHz);
            state.set(RITVALUE, ritHz, false);
            break;
        case '\x01':
            // RIT on/off
            if(payloadIn.at(02) == '\x01')
            {
                emit haveRitEnabled(true);
            } else {
                emit haveRitEnabled(false);
            }
            state.set(RITFUNC, (bool)payloadIn.at(02), false);
            break;
        case '\x02':
            // Delta TX setting on/off
            break;
        default:
            break;
    }
}

void rigCommander::parseATU()
{
    // qInfo(logRig()) << "Have ATU status from radio. Emitting.";
    // Expect:
    // [0]: 0x1c
    // [1]: 0x01
    // [2]: 0 = off, 0x01 = on, 0x02 = tuning in-progress
    emit haveATUStatus((unsigned char) payloadIn[2]);
    // This is a bool so any non-zero will mean enabled.
    state.set(TUNERFUNC, (bool)payloadIn[2], false);
}

void rigCommander::parsePTT()
{
    // read after payloadIn[02]

    if(payloadIn[2] == (char)0)
    {
        // PTT off
        emit havePTTStatus(false);
    } else {
        // PTT on
        emit havePTTStatus(true);
    }
    state.set(PTT,(bool)payloadIn[2],false);
}

void rigCommander::parseRegisters1A()
{
    // The simpler of the 1A stuff:

    // 1A 06: data mode on/off
    //    07: IP+ enable/disable
    //    00:   memory contents
    //    01:   band stacking memory contents (last freq used is stored here per-band)
    //    03: filter width
    //    04: AGC rate
    // qInfo(logRig()) << "Looking at register 1A :";
    // printHex(payloadIn, false, true);

    // "INDEX: 00 01 02 03 04 "
    // "DATA:  1a 06 01 03 fd " (data mode enabled, filter width 3 selected)

    switch(payloadIn[01])
    {
        case '\x00':
        {
            // Memory contents
            break;
        }
        case '\x01':
        {
            // band stacking register
            parseBandStackReg();
            break;
        }
        case '\x03':
        {
            quint16 calc;
            quint8 pass = bcdHexToUChar((quint8)payloadIn[2]);
            if (state.getChar(MODE) == modeAM) {
                calc = 200 + (pass * 200);
            }
            else if (pass <= 10)
            {
                calc = 50 + (pass * 50);
            }
            else {
                calc = 600 + ((pass - 10) * 100);
            }
            emit havePassband(calc);
            state.set(PASSBAND, calc, false);
            break;
        }
        case '\x04':
        {
            state.set(AGC, (quint8)payloadIn[2], false);
            break;
        }
        case '\x06':
        {
            // data mode
            // emit havedataMode( (bool) payloadIn[somebit])
            // index
            // 03 04
            // XX YY
            // XX = 00 (off) or 01 (on)
            // YY: filter selected, 01 through 03.;
            // if YY is 00 then XX was also set to 00
            emit haveDataMode((bool)payloadIn[03]);
            state.set(DATAMODE, (quint8)payloadIn[3], false);
            break;
        }
        case '\x07':
        {
            // IP+ status
            break;
        }
        case '\x09':
        {
            state.set(MUTEFUNC, (quint8)payloadIn[2], false);
        }
        default:
        {
            break;
        }
    }
}

void rigCommander::parseRegister1B()
{
    quint16 tone=0;
    bool tinv = false;
    bool rinv = false;

    switch(payloadIn[01])
    {
        case '\x00':
            // "Repeater tone"
            tone = decodeTone(payloadIn);
            emit haveTone(tone);
            state.set(CTCSS, tone, false);
            break;
        case '\x01':
            // "TSQL tone"
            tone = decodeTone(payloadIn);
            emit haveTSQL(tone);
            state.set(TSQL, tone, false);
            break;
        case '\x02':
            // DTCS (DCS)
            tone = decodeTone(payloadIn, tinv, rinv);
            emit haveDTCS(tone, tinv, rinv);
            state.set(DTCS, tone, false);
            break;
        case '\x07':
            // "CSQL code (DV mode)"
            tone = decodeTone(payloadIn);
            state.set(CSQL, tone, false);
            break;
        default:
            break;
    }
}

void rigCommander::parseRegister16()
{
    //"INDEX: 00 01 02 03 "
    //"DATA:  16 5d 00 fd "
    //               ^-- mode info here
    rptAccessTxRx ra;

    switch(payloadIn.at(1))
    {
        case '\x5d':
            emit haveRptAccessMode((rptAccessTxRx)payloadIn.at(2));
            break;
        case '\x02':
            // Preamp
            emit havePreamp((unsigned char)payloadIn.at(2));
            state.set(PREAMP, (quint8)payloadIn.at(2), false);
            break;
        case '\x22':
            emit haveNB(payloadIn.at(2) != 0);
            state.set(NBFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x40':
            emit haveNR(payloadIn.at(2) != 0);
            state.set(NRFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x41': // Auto notch
            state.set(ANFFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x42':
            state.set(TONEFUNC, payloadIn.at(2) != 0, false);
            if(payloadIn.at(2)==1)
            {
                ra = ratrTONEon;
            } else {
                ra = ratrTONEoff;
            }
            emit haveRptAccessMode(ra);
            break;
        case '\x43':
            state.set(TSQLFUNC, payloadIn.at(2) != 0, false);
            if(payloadIn.at(2)==1)
            {
                ra = ratrTSQLon;
            } else {
                ra = ratrTSQLoff;
            }
            emit haveRptAccessMode(ra);
            break;
        case '\x44':
            emit haveComp(payloadIn.at(2) != 0);
            state.set(COMPFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x45':
            emit haveMonitor(payloadIn.at(2) != 0);
            state.set(MONFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x46':
            emit haveVox(payloadIn.at(2) != 0);
            state.set(VOXFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x47':
            if (payloadIn.at(2) == '\00') {
                state.set(FBKINFUNC, false, false);
                state.set(SBKINFUNC, false, false);
            }
            else if (payloadIn.at(2) == '\01') {
                state.set(FBKINFUNC, false, false);
                state.set(SBKINFUNC, true, false);

            }
            else if (payloadIn.at(2) == '\02') {
                state.set(FBKINFUNC, true, false);
                state.set(SBKINFUNC, false, false);
            }
            emit haveCWBreakMode(payloadIn.at(2));
            break;
        case '\x48': // Manual Notch
            state.set(MNFUNC, payloadIn.at(2) != 0, false);
            break;
        case '\x50': // Dial lock
            state.set(LOCKFUNC, payloadIn.at(2) != 0, false);
            break;
        default:
            break;
    }
}

void rigCommander::parseBandStackReg()
{
    //qInfo(logRig()) << "Band stacking register response received: ";
    //printHex(payloadIn, false, true);

    // Reference output, 20 meters, regCode 01 (latest):
    // "INDEX: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 "
    // "DATA:  1a 01 05 01 60 03 23 14 00 00 03 10 00 08 85 00 08 85 fd "

    char band = payloadIn[2];
    char regCode = payloadIn[3];
    freqt freqs = parseFrequency(payloadIn, 7);
    //float freq = (float)freqs.MHzDouble;

    // The Band Stacking command returns the regCode in the position that VFO is expected.
    // As BSR is always on the active VFO, just set that.
    freqs.VFO = selVFO_t::activeVFO;

    bool dataOn = (payloadIn[11] & 0x10) >> 4; // not sure...
    char mode = payloadIn[9];
    char filter = payloadIn[10];
    // 09, 10 mode
    // 11 digit RH: data mode on (1) or off (0)
    // 11 digit LH: CTCSS 0 = off, 1 = TONE, 2 = TSQL

    // 12, 13 : tone freq setting
    // 14, 15 tone squelch freq setting
    // if more, memory name (label) ascii

    qInfo(logRig()) << "BSR in rigCommander: band: " << QString("%1").arg(band) << " regCode: " << (QString)regCode << " freq Hz: " << freqs.Hz << ", mode: " << (unsigned int)mode << ", filter: " << (unsigned int)filter << " data: " << dataOn;
    //qInfo(logRig()) << "mode: " << (QString)mode << " dataOn: " << dataOn;
    //qInfo(logRig()) << "Freq Hz: " << freqs.Hz;

    emit haveBandStackReg(freqs, mode, filter, dataOn);
}

void rigCommander::parseDetailedRegisters1A05()
{
    // It seems a lot of misc stuff is under this command and subcommand.
    // 1A 05 ...
    // 00 01 02 03 04 ...

    // 02 and 03 make up a BCD'd number:
    // 0001, 0002, 0003, ... 0101, 0102, 0103...

    // 04 is a typical single byte response
    // 04 05 is a typical 0-255 response

    // This file processes the registers which are radically different in each model.
    // It is a work in progress.
    // TODO: inputMod source and gain for models: 7700, and 7600

    int level = (100*bcdHexToUChar(payloadIn[4])) + bcdHexToUChar(payloadIn[5]);

    int subcmd = bcdHexToUChar(payloadIn[3]) + (100*bcdHexToUChar(payloadIn[2]));

    rigInput input;
    input = (rigInput)bcdHexToUChar(payloadIn[4]);
    int inputRaw = bcdHexToUChar(payloadIn[4]);

    switch(rigCaps.model)
    {
        case model9700:
            switch(subcmd)
            {

                case 72:
                    // course reference
                    emit haveRefAdjustCourse(  bcdHexToUChar(payloadIn[5]) + (100*bcdHexToUChar(payloadIn[4])) );
                    break;
                case 73:
                    // fine reference
                    emit haveRefAdjustFine( bcdHexToUChar(payloadIn[5]) + (100*bcdHexToUChar(payloadIn[4])) );
                    break;
                case 112:
                    emit haveACCGain(level, 5);
                    break;
                case 113:
                    emit haveUSBGain(level);
                    break;
                case 114:
                    emit haveLANGain(level);
                    break;
                case 115:
                    emit haveModInput(input, false);
                    break;
                case 116:
                    emit haveModInput(input, true);
                    break;
                default:
                    break;
            }
            break;
        case model7850:
            switch(subcmd)
            {
                case 63:
                    switch(inputRaw)
                    {
                        case 0:
                            input = inputMic;
                            break;
                        case 1:
                            input = inputACCA;
                            break;
                        case 2:
                            input = inputACCB;
                            break;
                        case 8:
                            input = inputUSB;
                            break;
                        case 9:
                            input = inputLAN;
                            break;
                        default:
                            input = inputUnknown;
                            break;
                    }
                    emit haveModInput(input, false);
                    break;
                case 64:
                    switch(inputRaw)
                    {
                        case 0:
                            input = inputMic;
                            break;
                        case 1:
                            input = inputACCA;
                            break;
                        case 2:
                            input = inputACCB;
                            break;
                        case 8:
                            input = inputUSB;
                            break;
                        case 9:
                            input = inputLAN;
                            break;
                        default:
                            input = inputUnknown;
                            break;
                    }
                    emit haveModInput(input, true);
                    break;
                case 58:
                    emit haveACCGain(level, 0);
                    break;
                case 59:
                    emit haveACCGain(level, 1);
                    break;
                case 61:
                    emit haveUSBGain(level);
                    break;
                case 62:
                    emit haveLANGain(level);
                    break;
                default:
                    break;
            }
            break;
        case model7610:
            switch(subcmd)
            {
                case 91:
                    emit haveModInput(input, false);
                    break;
                case 92:
                    emit haveModInput(input, true);
                    break;
                case 88:
                    emit haveACCGain(level, 5);
                    break;
                case 89:
                    emit haveUSBGain(level);
                    break;
                case 90:
                    emit haveLANGain(level);
                    break;
                case 228:
                    emit haveDashRatio(inputRaw);
                    break;
                case 275:
                    qDebug(logRig()) << "Got antenna type";
                    emit haveAntennaType(inputRaw);
                    state.set(ANTENNATYPE, inputRaw, false);
                default:
                    break;
            }
            return;
        case model7600:
            switch(subcmd)
            {
                case 30:
                    emit haveModInput(input, false);
                    break;
                case 31:
                    emit haveModInput(input, true);
                    break;
                case 29:
                    emit haveUSBGain(level);
                    break;
                default:
                    break;
            }
            return;
        case model7300:
            switch(subcmd)
            {
                case 64:
                    emit haveACCGain(level, 5);
                    break;
                case 65:
                    emit haveUSBGain(level);
                    break;
                case 66:
                    emit haveModInput(input, false);
                    break;
                case 67:
                    emit haveModInput(input, true);
                    break;
                default:
                    break;
            }
            return;
        case model7100:
            switch(subcmd)
            {
                case 87:
                    emit haveACCGain(level, 5);
                    break;
                case 89:
                    emit haveUSBGain(level);
                    break;
                case 90:
                    emit haveModInput(input, false);
                    break;
                case 91:
                    emit haveModInput(input, true);
                    break;
                default:
                    break;
            }
            break;
        case model705:
            switch(subcmd)
            {
                case 116:
                    emit haveUSBGain(level);
                    break;
                case 117:
                    emit haveLANGain(level);
                    break;
                case 118:
                    switch(inputRaw)
                    {
                        case 0:
                            input = inputMic;
                            break;
                        case 1:
                            input = inputUSB;
                            break;
                        case 3:
                            input = inputLAN;
                            break;
                        default:
                            input = inputUnknown;
                            break;
                    }
                    emit haveModInput(input, false);
                    break;
                case 119:
                    switch(inputRaw)
                    {
                        case 0:
                            input = inputMic;
                            break;
                        case 1:
                            input = inputUSB;
                            break;
                        case 3:
                            input = inputLAN;
                            break;
                        default:
                            input = inputUnknown;
                            break;
                    }
                    emit haveModInput(input, true);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
}

void rigCommander::parseWFData()
{
    freqt freqSpan;
    bool isSub;
    switch(payloadIn[1])
    {
        case 0:
            // Chunk of spectrum
            parseSpectrum();
            break;
        case 0x10:
            // confirming scope is on
            state.set(SCOPEFUNC, (bool)payloadIn[2], true);
            break;
        case 0x11:
            // confirming output enabled/disabled of wf data.
            break;
        case 0x14:
            // fixed or center
            emit haveSpectrumMode(static_cast<spectrumMode>((unsigned char)payloadIn[3]));
            // [1] 0x14
            // [2] 0x00
            // [3] 0x00 (center), 0x01 (fixed), 0x02, 0x03
            break;
        case 0x15:
            // read span in center mode
            // [1] 0x15
            // [2] to [8] is span encoded as a frequency
            isSub = payloadIn.at(2)==0x01;
            freqSpan = parseFrequency(payloadIn, 6);
            emit haveScopeSpan(freqSpan, isSub);
            //qInfo(logRig()) << "Received 0x15 center span data: for frequency " << freqSpan.Hz;
            //printHex(payloadIn, false, true);
            break;
        case 0x16:
            // read edge mode center in edge mode
            emit haveScopeEdge((char)payloadIn[2]);
            //qInfo(logRig()) << "Received 0x16 edge in center mode:";
            //printHex(payloadIn, false, true);
            // [1] 0x16
            // [2] 0x01, 0x02, 0x03: Edge 1,2,3
            break;
        case 0x17:
            // Hold status (only 9700?)
            qDebug(logRig()) << "Received 0x17 hold status - need to deal with this!";
            printHex(payloadIn, false, true);
            break;
        case 0x19:
            // scope reference level
            // [1] 0x19
            // [2] 0x00
            // [3] 10dB digit, 1dB digit
            // [4] 0.1dB digit, 0
            // [5] 0x00 = +, 0x01 = -
            parseSpectrumRefLevel();
            break;
        default:
            qInfo(logRig()) << "Unknown waveform data received: ";
            printHex(payloadIn, false, true);
            break;
    }
}


mode_info rigCommander::createMode(mode_kind m, unsigned char reg, QString name)
{
    mode_info mode;
    mode.mk = m;
    mode.reg = reg;
    mode.name = name;
    return mode;
}

centerSpanData rigCommander::createScopeCenter(centerSpansType s, QString name)
{
    centerSpanData csd;
    csd.cstype = s;
    csd.name = name;
    return csd;
}

void rigCommander::determineRigCaps()
{
    //TODO: Determine available bands (low priority, rig will reject out of band requests anyway)

    std::vector <bandType> standardHF;
    std::vector <bandType> standardVU;

    bandType bandDef6m = bandType(band6m, 50000000, 54000000, modeUSB);
    bandType bandDef10m = bandType(band10m, 28000000, 29700000, modeUSB);
    bandType bandDef12m = bandType(band12m, 24890000, 24990000, modeUSB);
    bandType bandDef15m = bandType(band15m, 21000000, 21450000, modeUSB);
    bandType bandDef17m = bandType(band17m, 18068000, 18168000, modeUSB);
    bandType bandDef20m = bandType(band20m, 14000000, 14350000, modeUSB);
    bandType bandDef30m = bandType(band30m, 10100000, 10150000, modeLSB);
    bandType bandDef40m = bandType(band40m, 7000000, 7300000, modeLSB);
    bandType bandDef60m = bandType(band60m, 5250000, 5450000, modeLSB);
    bandType bandDef80m = bandType(band80m, 3500000, 4000000, modeLSB);
    bandType bandDef160m = bandType(band160m, 1800000, 2000000, modeLSB);
    bandType bandDef630m = bandType(band630m, 493000, 595000, modeLSB);
    bandType bandDef2200m = bandType(band2200m, 135000, 138000, modeLSB);
    bandType bandDef2m = bandType(band2m, 144000000, 148000000, modeUSB);
    bandType bandDef4m = bandType(band4m, 70000000, 70500000, modeUSB);
    bandType bandDef70cm = bandType(band70cm, 420000000, 450000000, modeUSB);
    bandType bandDef23cm = bandType(band23cm, 1240000000, 1400000000, modeUSB);
    bandType bandDef13cm = bandType(band13cm, 2300000000, 2450000000, modeUSB);
    //bandType bandDef9cm = bandType(band9cm, 3300000000, 3500000000, modeUSB);
    bandType bandDef6cm = bandType(band6cm, 5650000000, 5925000000, modeUSB);
    bandType bandDef3cm = bandType(band3cm, 10000000000, 10500000000, modeUSB);

    bandType bandDefAir(bandAir, 108000000, 137000000, modeAM);
    bandType bandDefWFM(bandWFM, 88000000, 108000000, modeWFM);
    bandType bandDefGen(bandGen, 10000, 30000000, modeAM);


    standardHF = { bandDef160m, bandDef80m, bandDef60m, bandDef40m, bandDef30m,
        bandDef20m, bandDef17m, bandDef15m, bandDef12m, bandDef10m, bandDef6m };

    standardVU = { bandDef2m, bandDef70cm };



    std::vector <mode_info> commonModes;
    commonModes = { createMode(modeLSB, 0x00, "LSB"), createMode(modeUSB, 0x01, "USB"),
                    createMode(modeFM, 0x05, "FM"), createMode(modeAM, 0x02, "AM"),
                    createMode(modeCW, 0x03, "CW"), createMode(modeCW_R, 0x07, "CW-R"),
                    createMode(modeRTTY, 0x04, "RTTY"), createMode(modeRTTY_R, 0x08, "RTTY-R")
                  };



    rigCaps.model = model;
    rigCaps.civ = incomingCIVAddr;

    rigCaps.hasDD = false;
    rigCaps.hasDV = false;
    rigCaps.hasDataModes = true; // USB-D, LSB-D, etc
    rigCaps.hasATU = false;

    rigCaps.hasCTCSS = false;
    rigCaps.hasDTCS = false;

    rigCaps.hasTBPF = false;
    rigCaps.hasIFShift = false;

    rigCaps.spectSeqMax = 0;
    rigCaps.spectAmpMax = 0;
    rigCaps.spectLenMax = 0;
    rigCaps.scopeCenterSpans = { createScopeCenter(cs2p5k, "±2.5k"), createScopeCenter(cs5k, "±5k"),
                                 createScopeCenter(cs10k, "±10k"), createScopeCenter(cs25k, "±25k"),
                                 createScopeCenter(cs50k, "±50k"), createScopeCenter(cs100k, "±100k"),
                                 createScopeCenter(cs250k, "±250k"), createScopeCenter(cs500k, "±500k")
                               };

    
    rigCaps.hasFDcomms = true; // false for older radios

    // Clear inputs/preamps/attenuators lists in case we have re-connected.
    rigCaps.preamps.clear();
    rigCaps.attenuators.clear();
    rigCaps.inputs.clear();
    rigCaps.inputs.append(inputMic);

    rigCaps.hasAttenuator = true; // Verify that all recent rigs have attenuators
    rigCaps.attenuators.push_back('\x00');
    rigCaps.hasPreamp = true;
    rigCaps.preamps.push_back('\x00');

    rigCaps.hasAntennaSel = false;
    rigCaps.hasRXAntenna = false;
    rigCaps.hasAntennaType = false;
    rigCaps.hasTransmit = true;
    rigCaps.hasPTTCommand = true;
    rigCaps.useRTSforPTT = false;

    // Common, reasonable defaults for most supported HF rigs:
    rigCaps.bsr[band160m] = 0x01;
    rigCaps.bsr[band80m] = 0x02;
    rigCaps.bsr[band40m] = 0x03;
    rigCaps.bsr[band30m] = 0x04;
    rigCaps.bsr[band20m] = 0x05;
    rigCaps.bsr[band17m] = 0x06;
    rigCaps.bsr[band15m] = 0x07;
    rigCaps.bsr[band12m] = 0x08;
    rigCaps.bsr[band10m] = 0x09;
    rigCaps.bsr[band6m] = 0x10;
    rigCaps.bsr[bandGen] = 0x11;

    // Bands that seem to change with every model:
    rigCaps.bsr[band2m] = 0x00;
    rigCaps.bsr[band70cm] = 0x00;
    rigCaps.bsr[band23cm] = 0x00;

    // These bands generally aren't defined:
    rigCaps.bsr[band4m] = 0x00;
    rigCaps.bsr[band60m] = 0x00;
    rigCaps.bsr[bandWFM] = 0x00;
    rigCaps.bsr[bandAir] = 0x00;
    rigCaps.bsr[band630m] = 0x00;
    rigCaps.bsr[band2200m] = 0x00;

    switch(model){
        case model7300:
            rigCaps.modelName = QString("IC-7300");
            rigCaps.rigctlModel = 3073;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 11;
            rigCaps.spectAmpMax = 160;
            rigCaps.spectLenMax = 475;
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDef4m, bandDef630m, bandDef2200m, bandDefGen });
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x71");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x30");
            break;
        case modelR8600:
            rigCaps.modelName = QString("IC-R8600");
            rigCaps.rigctlModel = 3079;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 11;
            rigCaps.spectAmpMax = 160;
            rigCaps.spectLenMax = 475;
            rigCaps.inputs.clear();
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasTransmit = false;
            rigCaps.hasPTTCommand = false;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasDV = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x10');
            rigCaps.attenuators.push_back('\x20');
            rigCaps.attenuators.push_back('\x30');
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.hasAntennaSel = true;
            rigCaps.antennas = {0x00, 0x01, 0x02};
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDef23cm, bandDef4m, bandDef630m, bandDef2200m, bandDefGen });
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {
                                     createMode(modeWFM, 0x06, "WFM"), createMode(modeS_AMD, 0x11, "S-AM (D)"),
                                     createMode(modeS_AML, 0x14, "S-AM(L)"), createMode(modeS_AMU, 0x15, "S-AM(U)"),
                                     createMode(modeP25, 0x16, "P25"), createMode(modedPMR, 0x18, "dPMR"),
                                     createMode(modeNXDN_VN, 0x19, "NXDN-VN"), createMode(modeNXDN_N, 0x20, "NXDN-N"),
                                     createMode(modeDCR, 0x21, "DCR")});
            rigCaps.scopeCenterSpans.insert(rigCaps.scopeCenterSpans.end(), {createScopeCenter(cs1M, "±1M"), createScopeCenter(cs2p5M, "±2.5M")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x92");
            rigCaps.hasVFOMS = true; // not documented very well
            rigCaps.hasVFOAB = true; // so we just do both...
            break;
        case model9700:
            rigCaps.modelName = QString("IC-9700");
            rigCaps.rigctlModel = 3081;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 11;
            rigCaps.spectAmpMax = 160;
            rigCaps.spectLenMax = 475;
            rigCaps.inputs.append(inputLAN);
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasDD = true;
            rigCaps.hasDV = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x10');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardVU;
            rigCaps.bands.push_back(bandDef23cm);
            rigCaps.bsr[band23cm] = 0x03;
            rigCaps.bsr[band70cm] = 0x02;
            rigCaps.bsr[band2m] = 0x01;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modeDV, 0x17, "DV"),
                                                       createMode(modeDD, 0x22, "DD")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x01\x27");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = true;
            rigCaps.hasAdvancedRptrToneCmds = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x43");
            break;
        case model905:
            rigCaps.modelName = QString("IC-905");
            rigCaps.rigctlModel = 0;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 11;
            rigCaps.spectAmpMax = 160;
            rigCaps.spectLenMax = 475;
            rigCaps.inputs.append(inputLAN);
            rigCaps.inputs.append(inputUSB);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasDD = true;
            rigCaps.hasDV = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x10');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardVU;
            rigCaps.bands.push_back(bandDef23cm);
            rigCaps.bands.push_back(bandDef13cm);
            rigCaps.bands.push_back(bandDef6cm);
            rigCaps.bands.push_back(bandDef3cm);
            rigCaps.bsr[band2m] = 0x01;
            rigCaps.bsr[band70cm] = 0x02;
            rigCaps.bsr[band23cm] = 0x03;
            rigCaps.bsr[band13cm] = 0x04;
            rigCaps.bsr[band6cm] = 0x05;
            rigCaps.bsr[band3cm] = 0x06;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modeDV, 0x17, "DV"),
                                                       createMode(modeDD, 0x22, "DD"),
                                                       createMode(modeATV, 0x23, "ATV")
                                                       });

            rigCaps.scopeCenterSpans.insert(rigCaps.scopeCenterSpans.end(), {createScopeCenter(cs1M, "±1M"),
                                                                             createScopeCenter(cs2p5M, "±2.5M"),
                                                                             createScopeCenter(cs5M, "±5M"),
                                                                             createScopeCenter(cs10M, "±10M"),
                                                                             createScopeCenter(cs25M, "±25M")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x01\x42");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasAdvancedRptrToneCmds = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x46");
            break;
        case model910h:
            rigCaps.modelName = QString("IC-910H");
            rigCaps.rigctlModel = 3044;
            rigCaps.hasSpectrum = false;
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasDD = false;
            rigCaps.hasDV = false;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasATU = false;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x10' , '\x20', '\x30'});
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardVU;
            rigCaps.bands.push_back(bandDef23cm);
            rigCaps.bsr[band23cm] = 0x03;
            rigCaps.bsr[band70cm] = 0x02;
            rigCaps.bsr[band2m] = 0x01;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x58");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            break;
        case model7600:
            rigCaps.modelName = QString("IC-7600");
            rigCaps.rigctlModel = 3063;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputACC);
            rigCaps.inputs.append(inputUSB);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = false;
            rigCaps.hasDTCS = false;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(), {0x00, 0x06, 0x12, 0x18});
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), { createMode(modePSK, 0x12, "PSK"),
                                                       createMode(modePSK_R, 0x13, "PSK-R") });
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x97");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x64");
            break;
        case model7610:
            rigCaps.modelName = QString("IC-7610");
            rigCaps.rigctlModel = 3078;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 15;
            rigCaps.spectAmpMax = 200;
            rigCaps.spectLenMax = 689;
            rigCaps.inputs.append(inputLAN);
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasCTCSS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),
                                      {'\x03', '\x06', '\x09', '\x12',\
                                       '\x15', '\x18', '\x21', '\x24',\
                                       '\x27', '\x30', '\x33', '\x36',
                                       '\x39', '\x42', '\x45'});
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.hasAntennaSel = true;
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.hasATU = true;
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDef630m, bandDef2200m, bandDefGen });
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), { createMode(modePSK, 0x12, "PSK"),
                                                       createMode(modePSK_R, 0x13, "PSK-R") });
            rigCaps.hasRXAntenna = true;
            rigCaps.hasAntennaType = true;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x01\x12");
            rigCaps.hasSpecifyMainSubCmd = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x33");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            break;
        case model7850:
            rigCaps.modelName = QString("IC-785x");
            rigCaps.rigctlModel = 3075;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 15;
            rigCaps.spectAmpMax = 136;
            rigCaps.spectLenMax = 689;
            rigCaps.inputs.append(inputLAN);
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACCA);
            rigCaps.inputs.append(inputACCB);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),
                                      {'\x03', '\x06', '\x09',
                                       '\x12', '\x15', '\x18', '\x21'});
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.hasAntennaSel = true;
            rigCaps.antennas = {0x00, 0x01, 0x02, 0x03};
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDef630m, bandDef2200m, bandDefGen });
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modePSK, 0x12, "PSK"),
                                                       createMode(modePSK_R, 0x13, "PSK-R")});
            rigCaps.hasRXAntenna = true;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x01\x55");
            rigCaps.hasSpecifyMainSubCmd = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x01\x13");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            break;
        case model705:
            rigCaps.modelName = QString("IC-705");
            rigCaps.rigctlModel = 3085;
            rigCaps.hasSpectrum = true;
            rigCaps.spectSeqMax = 11;
            rigCaps.spectAmpMax = 160;
            rigCaps.spectLenMax = 475;
            rigCaps.inputs.append(inputLAN);
            rigCaps.inputs.append(inputUSB);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = true;
            rigCaps.hasDD = true;
            rigCaps.hasDV = true;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x10' , '\x20'});
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDefAir, bandDefGen, bandDefWFM, bandDef630m, bandDef2200m });
            rigCaps.bsr[band70cm] = 0x14;
            rigCaps.bsr[band2m] = 0x13;
            rigCaps.bsr[bandAir] = 0x12;
            rigCaps.bsr[bandWFM] = 0x11;
            rigCaps.bsr[bandGen] = 0x15;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modeWFM, 0x06, "WFM"),
                                                       createMode(modeDV, 0x17, "DV")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x01\x31");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x45");
            break;
        case model7000:
            rigCaps.modelName = QString("IC-7000");
            rigCaps.rigctlModel = 3060;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x12');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[band2m] = 0x11;
            rigCaps.bsr[band70cm] = 0x12;
            rigCaps.bsr[bandGen] = 0x13;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x92");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x52");
            break;
        case model7410:
            rigCaps.modelName = QString("IC-7410");
            rigCaps.rigctlModel = 3067;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = true;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x40");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x11");
            break;
        case model7100:
            rigCaps.modelName = QString("IC-7100");
            rigCaps.rigctlModel = 3070;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x12');
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDef4m, bandDefGen});
            rigCaps.bsr[band2m] = 0x11;
            rigCaps.bsr[band70cm] = 0x12;
            rigCaps.bsr[bandGen] = 0x13;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modeWFM, 0x06, "WFM"),
                                                       createMode(modeDV, 0x17, "DV")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x95");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x15");
            break;
        case model7200:
            rigCaps.modelName = QString("IC-7200");
            rigCaps.rigctlModel = 3061;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputUSB);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x03\x48");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x03\x18");
            break;
        case model7700:
            rigCaps.modelName = QString("IC-7700");
            rigCaps.rigctlModel = 3062;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputLAN);
            //rigCaps.inputs.append(inputSPDIF);
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = true;
            rigCaps.hasEthernet = true;
            rigCaps.hasWiFi = false;
            rigCaps.hasCTCSS = true;
            rigCaps.hasTBPF = true;
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),
                                      {'\x06', '\x12', '\x18'});
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.hasAntennaSel = true;
            rigCaps.antennas = {0x00, 0x01, 0x02, 0x03}; // not sure if 0x03 works
            rigCaps.hasATU = true;
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), { bandDefGen, bandDef630m, bandDef2200m });
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modePSK, 0x12, "PSK"),
                                                       createMode(modePSK_R, 0x13, "PSK-R")});
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x95");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x67");
            break;
        case model703:
            rigCaps.modelName = QString("IC-703");
            rigCaps.rigctlModel = 3055;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), createMode(modeWFM, 0x06, "WFM"));
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            break;
        case model706:
            rigCaps.modelName = QString("IC-706");
            rigCaps.rigctlModel = 3009;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), createMode(modeWFM, 0x06, "WFM"));
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            break;
        case model718:
            rigCaps.modelName = QString("IC-718");
            rigCaps.rigctlModel = 3013;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = false;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasIFShift = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands =   {bandDef10m, bandDef10m, bandDef12m,
                               bandDef15m, bandDef17m, bandDef20m, bandDef30m,
                               bandDef40m, bandDef60m, bandDef80m, bandDef160m, bandDefGen};
            rigCaps.modes = { createMode(modeLSB, 0x00, "LSB"), createMode(modeUSB, 0x01, "USB"),
                              createMode(modeAM, 0x02, "AM"),
                              createMode(modeCW, 0x03, "CW"), createMode(modeCW_R, 0x07, "CW-R"),
                              createMode(modeRTTY, 0x04, "RTTY"), createMode(modeRTTY_R, 0x08, "RTTY-R")
                            };
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
        break;
        case model736:
            rigCaps.modelName = QString("IC-736");
            rigCaps.rigctlModel = 3020;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = false;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardHF;
            rigCaps.modes = { createMode(modeLSB, 0x00, "LSB"), createMode(modeUSB, 0x01, "USB"),
                              createMode(modeAM, 0x02, "AM"), createMode(modeFM, 0x05, "FM"),
                              createMode(modeCW, 0x03, "CW"), createMode(modeCW_R, 0x07, "CW-R"),
                            };
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            break;
        case model737:
            rigCaps.modelName = QString("IC-737");
            rigCaps.rigctlModel = 3021;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = false;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardHF;
            rigCaps.modes = { createMode(modeLSB, 0x00, "LSB"), createMode(modeUSB, 0x01, "USB"),
                              createMode(modeAM, 0x02, "AM"), createMode(modeFM, 0x05, "FM"),
                              createMode(modeCW, 0x03, "CW"), createMode(modeCW_R, 0x07, "CW-R"),
                            };
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            break;
        case model738:
            rigCaps.modelName = QString("IC-738");
            rigCaps.rigctlModel = 3022;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = false;
            rigCaps.hasPTTCommand = false;
            rigCaps.useRTSforPTT = true;
            rigCaps.hasDataModes = false;
            rigCaps.attenuators.push_back('\x20');
            rigCaps.preamps.push_back('\x01');
            rigCaps.bands = standardHF;
            rigCaps.modes = { createMode(modeLSB, 0x00, "LSB"), createMode(modeUSB, 0x01, "USB"),
                              createMode(modeAM, 0x02, "AM"), createMode(modeFM, 0x05, "FM"),
                              createMode(modeCW, 0x03, "CW"), createMode(modeCW_R, 0x07, "CW-R"),
                            };
            rigCaps.hasVFOMS = false;
            rigCaps.hasVFOAB = true;
            break;
        case model746:
            rigCaps.modelName = QString("IC-746");
            rigCaps.rigctlModel = 3023;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasTBPF = true;
            rigCaps.hasIFShift = true;
            rigCaps.hasCTCSS = true;
            rigCaps.hasDTCS = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.hasAntennaSel = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x20'});
            // There are two HF and VHF ant, 12-01 adn 12-02 select the HF, the VHF is auto selected
            // this incorrectly shows up as 2 and 3 in the drop down.
            rigCaps.antennas = {0x01, 0x02};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDef2m);
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = true;
            break;
        case model756:
            rigCaps.modelName = QString("IC-756");
            rigCaps.rigctlModel = 3026;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasTBPF = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x06' , '\x12', '\x18'});
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            break;
        case model756pro:
            rigCaps.modelName = QString("IC-756 Pro");
            rigCaps.rigctlModel = 3027;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasTBPF = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x06' , '\x12', '\x18'});
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            break;
        case model756proii:
            rigCaps.modelName = QString("IC-756 Pro II");
            rigCaps.rigctlModel = 3027;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasTBPF = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x06' , '\x12', '\x18'});
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x24");
            break;
        case model756proiii:
            rigCaps.modelName = QString("IC-756 Pro III");
            rigCaps.rigctlModel = 3027;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasTBPF = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x06' , '\x12', '\x18'});
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[bandGen] = 0x11;
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = false;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x24");
            break;
        case model9100:
            rigCaps.modelName = QString("IC-9100");
            rigCaps.rigctlModel = 3068;
            rigCaps.hasSpectrum = false;
            rigCaps.inputs.append(inputUSB); // TODO, add commands for this radio's inputs
            rigCaps.inputs.append(inputACC);
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasATU = true;
            rigCaps.hasDV = true;
            rigCaps.hasTBPF = true;
            rigCaps.hasRepeaterModes = true;
            rigCaps.preamps.push_back('\x01');
            rigCaps.preamps.push_back('\x02');
            rigCaps.attenuators.insert(rigCaps.attenuators.end(),{ '\x20' });
            rigCaps.antennas = {0x00, 0x01};
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.push_back(bandDef23cm);
            rigCaps.bands.push_back(bandDefGen);
            rigCaps.bsr[band2m] = 0x11;
            rigCaps.bsr[band70cm] = 0x12;
            rigCaps.bsr[band23cm] = 0x13;
            rigCaps.bsr[bandGen] = 0x14;
            rigCaps.modes = commonModes;
            rigCaps.modes.insert(rigCaps.modes.end(), {createMode(modeDV, 0x17, "DV")});
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = true;
            rigCaps.hasQuickSplitCommand = true;
            rigCaps.quickSplitCommand = QByteArrayLiteral("\x1a\x05\x00\x14");
            break;
        default:
            rigCaps.modelName = QString("IC-0x%1").arg(rigCaps.modelID, 2, 16);
            rigCaps.hasSpectrum = false;
            rigCaps.spectSeqMax = 0;
            rigCaps.spectAmpMax = 0;
            rigCaps.spectLenMax = 0;
            rigCaps.inputs.clear();
            rigCaps.hasLan = false;
            rigCaps.hasEthernet = false;
            rigCaps.hasWiFi = false;
            rigCaps.hasFDcomms = false;
            rigCaps.hasPreamp = false;
            rigCaps.hasAntennaSel = false;
            rigCaps.attenuators.push_back('\x10');
            rigCaps.attenuators.push_back('\x12');
            rigCaps.attenuators.push_back('\x20');
            rigCaps.bands = standardHF;
            rigCaps.bands.insert(rigCaps.bands.end(), standardVU.begin(), standardVU.end());
            rigCaps.bands.insert(rigCaps.bands.end(), {bandDef23cm, bandDef4m, bandDef630m, bandDef2200m, bandDefGen});
            rigCaps.modes = commonModes;
            rigCaps.transceiveCommand = QByteArrayLiteral("\x1a\x05\x00\x00");
            rigCaps.hasVFOMS = true;
            rigCaps.hasVFOAB = true;
            qInfo(logRig()) << "Found unknown rig: 0x" << QString("%1").arg(rigCaps.modelID, 2, 16);
            break;
    }
    haveRigCaps = true;

    // Copy received guid so we can recognise this radio.
    memcpy(rigCaps.guid, this->guid, GUIDLEN);

    if(!usingNativeLAN)
    {
        if(useRTSforPTT_isSet)
        {
            rigCaps.useRTSforPTT = useRTSforPTT_manual;
        }
        comm->setUseRTSforPTT(rigCaps.useRTSforPTT);
    }

    if(lookingForRig)
    {
        lookingForRig = false;
        foundRig = true;

        qDebug(logRig()) << "---Rig FOUND from broadcast query:";
        this->civAddr = incomingCIVAddr; // Override and use immediately.
        payloadPrefix = QByteArray("\xFE\xFE");
        payloadPrefix.append(civAddr);
        payloadPrefix.append((char)compCivAddr);
        // if there is a compile-time error, remove the following line, the "hex" part is the issue:
        qInfo(logRig()) << "Using incomingCIVAddr: (int): " << this->civAddr << " hex: " << QString("0x%1").arg(this->civAddr,0,16);
        emit discoveredRigID(rigCaps);
    } else {
        if(!foundRig)
        {
            emit discoveredRigID(rigCaps);
            foundRig = true;
        }
        emit haveRigID(rigCaps);
    }
}

void rigCommander::parseSpectrum()
{
    if(!haveRigCaps)
    {
        qDebug(logRig()) << "Spectrum received in rigCommander, but rigID is incomplete.";
        return;
    }
    if(rigCaps.spectSeqMax == 0)
    {
        // there is a chance this will happen with rigs that support spectrum. Once our RigID query returns, we will parse correctly.
        qInfo(logRig()) << "Warning: Spectrum sequence max was zero, yet spectrum was received.";
        return;
    }
    // Here is what to expect:
    // payloadIn[00] = '\x27';
    // payloadIn[01] = '\x00';
    // payloadIn[02] = '\x00';
    //
    // Example long: (sequences 2-10, 50 pixels)
    // "INDEX: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 "
    // "DATA:  27 00 00 07 11 27 13 15 01 00 22 21 09 08 06 19 0e 20 23 25 2c 2d 17 27 29 16 14 1b 1b 21 27 1a 18 17 1e 21 1b 24 21 22 23 13 19 23 2f 2d 25 25 0a 0e 1e 20 1f 1a 0c fd "
    //                  ^--^--(seq 7/11)
    //                        ^-- start waveform data 0x00 to 0xA0, index 05 to 54
    //

    // Example medium: (sequence #11)
    // "INDEX: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 "
    // "DATA:  27 00 00 11 11 0b 13 21 23 1a 1b 22 1e 1a 1d 13 21 1d 26 28 1f 19 1a 18 09 2c 2c 2c 1a 1b fd "

    // Example short: (sequence #1) includes center/fixed mode at [05]. No pixels.
    // "INDEX: 00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 "
    // "DATA:  27 00 00 01 11 01 00 00 00 14 00 00 00 35 14 00 00 fd "
    //                        ^-- mode 00 (center) or 01 (fixed)
    //                                     ^--14.00 MHz lower edge
    //                                                    ^-- 14.350 MHz upper edge
    //                                                          ^-- possibly 00=in range 01 = out of range

    // Note, the index used here, -1, matches the ICD in the owner's manual.
    // Owner's manual + 1 = our index.

    // divs: Mode: Waveinfo: Len:   Comment:
    // 2-10  var   var       56     Minimum wave information w/waveform data
    // 11    10    26        31     Minimum wave information w/waveform data
    // 1     1     0         18     Only Wave Information without waveform data

    freqt fStart;
    freqt fEnd;

    unsigned char vfo = bcdHexToUChar(payloadIn[02]);
    unsigned char sequence = bcdHexToUChar(payloadIn[03]);

    //unsigned char sequenceMax = bcdHexToDecimal(payloadIn[04]);

    if (vfo == 1)
    {
        // This is for the second VFO!
        return;
    }

    // unsigned char waveInfo = payloadIn[06]; // really just one byte?
    //qInfo(logRig()) << "Spectrum Data received: " << sequence << "/" << sequenceMax << " mode: " << scopeMode << " waveInfo: " << waveInfo << " length: " << payloadIn.length();

    // Sequnce 2, index 05 is the start of data
    // Sequence 11. index 05, is the last chunk
    // Sequence 11, index 29, is the actual last pixel (it seems)

    // It looks like the data length may be variable, so we need to detect it each time.
    // start at payloadIn.length()-1 (to override the FD). Never mind, index -1 bad.
    // chop off FD.
    if ((sequence == 1) && (sequence < rigCaps.spectSeqMax))
    {

        spectrumMode scopeMode = (spectrumMode)bcdHexToUChar(payloadIn[05]); // 0=center, 1=fixed

        if(scopeMode != oldScopeMode)
        {
            //TODO: support the other two modes (firmware 1.40)
            // Modes:
            // 0x00 Center
            // 0x01 Fixed
            // 0x02 Scroll-C
            // 0x03 Scroll-F
            emit haveSpectrumMode(scopeMode);
            oldScopeMode = scopeMode;
        }

        if(payloadIn.length() >= 15)
        {
            bool outOfRange = (bool)payloadIn[16];
            if(outOfRange != wasOutOfRange)
            {
                emit haveScopeOutOfRange(outOfRange);
                wasOutOfRange = outOfRange;
                return;
            }
        }

        // wave information
        spectrumLine.clear();
        // For Fixed, and both scroll modes, the following produces correct information:
        fStart = parseFrequency(payloadIn, 9);
        spectrumStartFreq = fStart.MHzDouble;
        fEnd = parseFrequency(payloadIn, 14);
        spectrumEndFreq = fEnd.MHzDouble;
        if(scopeMode == spectModeCenter)
        {
            // "center" mode, start is actual center, end is bandwidth.
            spectrumStartFreq -= spectrumEndFreq;
            spectrumEndFreq = spectrumStartFreq + 2*(spectrumEndFreq);
            // emit haveSpectrumCenterSpan(span);
        }

        if (payloadIn.length() > 400) // Must be a LAN packet.
        {
            payloadIn.chop(1);
            //spectrumLine.append(payloadIn.mid(17,475)); // write over the FD, last one doesn't, oh well.
            spectrumLine.append(payloadIn.right(payloadIn.length()-17)); // write over the FD, last one doesn't, oh well.
            emit haveSpectrumData(spectrumLine, spectrumStartFreq, spectrumEndFreq);
        }
    } else if ((sequence > 1) && (sequence < rigCaps.spectSeqMax))
    {
        // spectrum from index 05 to index 54, length is 55 per segment. Length is 56 total. Pixel data is 50 pixels.
        // sequence numbers 2 through 10, 50 pixels each. Total after sequence 10 is 450 pixels.
        payloadIn.chop(1);
        spectrumLine.insert(spectrumLine.length(), payloadIn.right(payloadIn.length() - 5)); // write over the FD, last one doesn't, oh well.
        //qInfo(logRig()) << "sequence: " << sequence << "spec index: " << (sequence-2)*55 << " payloadPosition: " << payloadIn.length() - 5 << " payload length: " << payloadIn.length();
    } else if (sequence == rigCaps.spectSeqMax)
    {
        // last spectrum, a little bit different (last 25 pixels). Total at end is 475 pixels (7300).
        payloadIn.chop(1);
        spectrumLine.insert(spectrumLine.length(), payloadIn.right(payloadIn.length() - 5));
        //qInfo(logRig()) << "sequence: " << sequence << " spec index: " << (sequence-2)*55 << " payloadPosition: " << payloadIn.length() - 5 << " payload length: " << payloadIn.length();
        emit haveSpectrumData(spectrumLine, spectrumStartFreq, spectrumEndFreq);
    }
}

void rigCommander::parseSpectrumRefLevel()
{
    // 00: 27
    // 01: 19
    // 02: 00 (fixed)
    // 03: XX
    // 04: x0
    // 05: 00 (+) or 01 (-)

    unsigned char negative = payloadIn[5];
    int value = bcdHexToUInt(payloadIn[3], payloadIn[4]);
    value = value / 10;
    if(negative){
        value *= (-1*negative);
    }
    emit haveSpectrumRefLevel(value);
}

unsigned char rigCommander::bcdHexToUChar(unsigned char in)
{
    unsigned char out = 0;
    out = in & 0x0f;
    out += ((in & 0xf0) >> 4)*10;
    return out;
}

unsigned int rigCommander::bcdHexToUInt(unsigned char hundreds, unsigned char tensunits)
{
    // convert:
    // hex data: 0x41 0x23
    // convert to uint:
    // uchar: 4123
    unsigned char thousands = ((hundreds & 0xf0)>>4);
    unsigned int rtnVal;
    rtnVal = (hundreds & 0x0f)*100;
    rtnVal += ((tensunits & 0xf0)>>4)*10;
    rtnVal += (tensunits & 0x0f);
    rtnVal += thousands * 1000;

    return rtnVal;
}

unsigned char rigCommander::bcdHexToUChar(unsigned char hundreds, unsigned char tensunits)
{
    // convert:
    // hex data: 0x01 0x23
    // convert to uchar:
    // uchar: 123

    //unsigned char thousands = ((hundreds & 0xf0)>>4);
    unsigned char rtnVal;
    rtnVal = (hundreds & 0x0f)*100;
    rtnVal += ((tensunits & 0xf0)>>4)*10;
    rtnVal += (tensunits & 0x0f);
    //rtnVal += thousands * 1000;

    return rtnVal;
}

QByteArray rigCommander::bcdEncodeInt(unsigned int num)
{
    if(num > 9999)
    {
        qInfo(logRig()) << __FUNCTION__ << "Error, number is too big for four-digit conversion: " << num;
        return QByteArray();
    }

    char thousands = num / 1000;
    char hundreds = (num - (1000*thousands)) / 100;
    char tens = (num - (1000*thousands) - (100*hundreds)) / 10;
    char units = (num - (1000*thousands) - (100*hundreds) - (10*tens));

    char b0 = hundreds | (thousands << 4);
    char b1 = units | (tens << 4);

    //qInfo(logRig()) << __FUNCTION__ << " encoding value " << num << " as hex:";
    //printHex(QByteArray(b0), false, true);
    //printHex(QByteArray(b1), false, true);


    QByteArray result;
    result.append(b0).append(b1);
    return result;
}

void rigCommander::parseFrequency()
{
    freqt freq;
    freq.Hz = 0;
    freq.MHzDouble = 0;

    // process payloadIn, which is stripped.
    // float frequencyMhz
    //    payloadIn[04] = ; // XX MHz
    //    payloadIn[03] = ; //   XX0     KHz
    //    payloadIn[02] = ; //     X.X   KHz
    //    payloadIn[01] = ; //      . XX KHz

    // printHex(payloadIn, false, true);

    frequencyMhz = 0.0;
    if (payloadIn.length() == 7)
    {
        // 7300 has these digits too, as zeros.
        // IC-705 or IC-9700 with higher frequency data available.
        frequencyMhz += 100 * (payloadIn[05] & 0x0f);
        frequencyMhz += (1000 * ((payloadIn[05] & 0xf0) >> 4));

        freq.Hz += (payloadIn[05] & 0x0f) * 1E6 * 100;
        freq.Hz += ((payloadIn[05] & 0xf0) >> 4) * 1E6 * 1000;

    }

    freq.Hz += (payloadIn[04] & 0x0f) * 1E6;
    freq.Hz += ((payloadIn[04] & 0xf0) >> 4) * 1E6 * 10;

    frequencyMhz += payloadIn[04] & 0x0f;
    frequencyMhz += 10 * ((payloadIn[04] & 0xf0) >> 4);

    // KHz land:
    frequencyMhz += ((payloadIn[03] & 0xf0) >> 4) / 10.0;
    frequencyMhz += (payloadIn[03] & 0x0f) / 100.0;

    frequencyMhz += ((payloadIn[02] & 0xf0) >> 4) / 1000.0;
    frequencyMhz += (payloadIn[02] & 0x0f) / 10000.0;

    frequencyMhz += ((payloadIn[01] & 0xf0) >> 4) / 100000.0;
    frequencyMhz += (payloadIn[01] & 0x0f) / 1000000.0;

    freq.Hz += payloadIn[01] & 0x0f;
    freq.Hz += ((payloadIn[01] & 0xf0) >> 4) * 10;

    freq.Hz += (payloadIn[02] & 0x0f) * 100;
    freq.Hz += ((payloadIn[02] & 0xf0) >> 4) * 1000;

    freq.Hz += (payloadIn[03] & 0x0f) * 10000;
    freq.Hz += ((payloadIn[03] & 0xf0) >> 4) * 100000;

    freq.MHzDouble = frequencyMhz;
    
    if (state.getChar(CURRENTVFO) == 0) {
        state.set(VFOAFREQ, freq.Hz, false);
    }
    else {
        state.set(VFOBFREQ, freq.Hz, false);
    }
    
    emit haveFrequency(freq);
}

freqt rigCommander::parseFrequencyRptOffset(QByteArray data)
{
    // VHF 600 KHz:
    // DATA:  0c 00 60 00 fd
    // INDEX: 00 01 02 03 04

    // UHF 5 MHz:
    // DATA:  0c 00 00 05 fd
    // INDEX: 00 01 02 03 04

    freqt f;
    f.Hz = 0;

    f.Hz += (data[3] & 0x0f)        *    1E6; // 1 MHz
    f.Hz += ((data[3] & 0xf0) >> 4) *    1E6 * 10; //   10 MHz
    f.Hz += (data[2] & 0x0f) *          10E3; // 10 KHz
    f.Hz += ((data[2] & 0xf0) >> 4) *  100E3; // 100 KHz
    f.Hz += (data[1] & 0x0f) *           100; // 100 Hz
    f.Hz += ((data[1] & 0xf0) >> 4) *   1000; // 1 KHz

    f.MHzDouble=f.Hz/1E6;
    f.VFO = activeVFO;
    return f;
}

freqt rigCommander::parseFrequency(QByteArray data, unsigned char lastPosition)
{
    // process payloadIn, which is stripped.
    // float frequencyMhz
    //    payloadIn[04] = ; // XX MHz
    //    payloadIn[03] = ; //   XX0     KHz
    //    payloadIn[02] = ; //     X.X   KHz
    //    payloadIn[01] = ; //      . XX KHz

    //printHex(data, false, true);

    // TODO: Check length of data array prior to reading +/- position

    // NOTE: This function was written on the IC-7300, which has no need for 100 MHz and 1 GHz.
    //       Therefore, this function has to go to position +1 to retrieve those numbers for the IC-9700.

    freqt freqs;
    freqs.MHzDouble = 0;
    freqs.Hz = 0;

    // Does Frequency contain 100 MHz/1 GHz data?
    if(data.length() >= lastPosition+1)
    {
        freqs.Hz += (data[lastPosition+1] & 0x0f) * 1E6 *         100; //  100 MHz
        freqs.Hz += ((data[lastPosition+1] & 0xf0) >> 4) * 1E6 * 1000; // 1000 MHz
    }

    // Does Frequency contain VFO data? (\x25 command)
    if (lastPosition-4 >= 0 && (quint8)data[lastPosition-4] < 0x02)
    {
        freqs.VFO=(selVFO_t)(quint8)data[lastPosition-4];
    }

    freqs.Hz += (data[lastPosition] & 0x0f) * 1E6;
    freqs.Hz += ((data[lastPosition] & 0xf0) >> 4) * 1E6 *     10; //   10 MHz

    freqs.Hz += (data[lastPosition-1] & 0x0f) *          10E3; // 10 KHz
    freqs.Hz += ((data[lastPosition-1] & 0xf0) >> 4) *  100E3; // 100 KHz

    freqs.Hz += (data[lastPosition-2] & 0x0f) *           100; // 100 Hz
    freqs.Hz += ((data[lastPosition-2] & 0xf0) >> 4) *   1000; // 1 KHz

    freqs.Hz += (data[lastPosition-3] & 0x0f) *             1; // 1 Hz
    freqs.Hz += ((data[lastPosition-3] & 0xf0) >> 4) *     10; // 10 Hz

    freqs.MHzDouble = (double)(freqs.Hz / 1000000.0);
    return freqs;
}


void rigCommander::parseMode()
{
    unsigned char filter;
    if(payloadIn[2] != '\xFD')
    {
        filter = payloadIn[2];
    } else {
        filter = 0;
    }
    unsigned char mode = (unsigned char)payloadIn[01];
    emit haveMode(mode, filter);
    state.set(MODE,mode,false);
    state.set(FILTER, filter, false);
    quint16 pass = 0;

    if (!state.isValid(PASSBAND)) {
 
        /*  We haven't got a valid passband from the rig so we 
            need to create a 'fake' one from default values
            This will be replaced with a valid one if we get it */

        if (mode == 3 || mode == 7 || mode == 12 || mode == 17) {
            switch (filter) {
            case 1:
                pass=1200;
                break;
            case 2:
                pass=500;
                break;
            case 3:
                pass=250;
                break;
            }
        }
        else if (mode == 4 || mode == 8)
        {
            switch (filter) {
            case 1:
                pass=2400;
                break;
            case 2:
                pass=500;
                break;
            case 3:
                pass=250;
                break;
            }
        }
        else if (mode == 2)
        {
            switch (filter) {
            case 1:
                pass=9000;
                break;
            case 2:
                pass=6000;
                break;
            case 3:
                pass=3000;
                break;
            }
        }
        else if (mode == 5)
        {
            switch (filter) {
            case 1:
                pass=15000;
                break;
            case 2:
                pass=10000;
                break;
            case 3:
                pass=7000;
                break;
            }
        }
        else { // SSB or unknown mode
            switch (filter) {
            case 1:
                pass=3000;
                break;
            case 2:
                pass=2400;
                break;
            case 3:
                pass=1800;
                break;
            }
        }
    }
    state.set(PASSBAND, pass, false);
}


void rigCommander::startATU()
{
    QByteArray payload("\x1C\x01\x02");
    prepDataAndSend(payload);
}

void rigCommander::setATU(bool enabled)
{
    QByteArray payload;

    if(enabled)
    {
        payload.setRawData("\x1C\x01\x01", 3);
    } else {
        payload.setRawData("\x1C\x01\x00", 3);
    }
    prepDataAndSend(payload);
}

void rigCommander::getATUStatus()
{
    //qInfo(logRig()) << "Sending out for ATU status in RC.";
    QByteArray payload("\x1C\x01");
    prepDataAndSend(payload);
}

void rigCommander::getAttenuator()
{
    QByteArray payload("\x11");
    prepDataAndSend(payload);
}

void rigCommander::getPreamp()
{
    QByteArray payload("\x16\x02");
    prepDataAndSend(payload);
}

void rigCommander::getAntenna()
{
    // This one might need some thought
    // as it seems each antenna has to be checked.
    // Maybe 0x12 alone will do it.
    QByteArray payload("\x12");
    prepDataAndSend(payload);
}

void rigCommander::getAntennaType()
{
    QByteArray payload("\x1a\x05\x02\x75");
    prepDataAndSend(payload);
}
void rigCommander::setAttenuator(unsigned char att)
{
    QByteArray payload("\x11");
    payload.append(att);
    prepDataAndSend(payload);
}

void rigCommander::setPreamp(unsigned char pre)
{
    QByteArray payload("\x16\x02");
    payload.append(pre);
    prepDataAndSend(payload);
}

void rigCommander::setAntenna(unsigned char ant, bool rx)
{
    QByteArray payload("\x12");
    payload.append(ant);
    if (rigCaps.hasRXAntenna) {
        payload.append((unsigned char)rx); // 0x00 = use for TX and RX
    }
    prepDataAndSend(payload);
}

void rigCommander::setAntennaType(unsigned char typ)
{
    QByteArray payload("\x1a\x05\x02\x75");
    payload.append(typ);
    prepDataAndSend(payload);
}
void rigCommander::setNB(bool enabled) {
    QByteArray payload("\x16\x22");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getNB()
{
    QByteArray payload;
    payload.setRawData("\x16\x22", 2);
    prepDataAndSend(payload);
}

void rigCommander::setNR(bool enabled) {
    QByteArray payload("\x16\x40");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getNR()
{
    QByteArray payload;
    payload.setRawData("\x16\x40", 2);
    prepDataAndSend(payload);
}

void rigCommander::setAutoNotch(bool enabled)
{
    QByteArray payload("\x16\x41");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getAutoNotch()
{
    QByteArray payload;
    payload.setRawData("\x16\x41", 2);
    prepDataAndSend(payload);
}

void rigCommander::setToneEnabled(bool enabled)
{
    QByteArray payload("\x16\x42");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getToneEnabled()
{
    QByteArray payload;
    payload.setRawData("\x16\x42", 2);
    prepDataAndSend(payload);
}

void rigCommander::setToneSql(bool enabled)
{
    QByteArray payload("\x16\x43");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getToneSqlEnabled()
{
    QByteArray payload;
    payload.setRawData("\x16\x43", 2);
    prepDataAndSend(payload);
}

void rigCommander::setCompressor(bool enabled)
{
    QByteArray payload("\x16\x44");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getCompressor()
{
    QByteArray payload;
    payload.setRawData("\x16\x44", 2);
    prepDataAndSend(payload);
}

void rigCommander::setMonitor(bool enabled)
{
    QByteArray payload("\x16\x45");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getMonitor()
{
    QByteArray payload;
    payload.setRawData("\x16\x45", 2);
    prepDataAndSend(payload);
}

void rigCommander::setVox(bool enabled)
{
    QByteArray payload("\x16\x46");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getVox()
{
    QByteArray payload;
    payload.setRawData("\x16\x46", 2);
    prepDataAndSend(payload);
}

void rigCommander::setBreakIn(unsigned char type) {
    QByteArray payload("\x16\x47");
    payload.append((unsigned char)type);
    prepDataAndSend(payload);
}

void rigCommander::getBreakIn()
{
    QByteArray payload;
    payload.setRawData("\x16\x47", 2);
    prepDataAndSend(payload);
}

void rigCommander::setKeySpeed(unsigned char wpm)
{
    // 0 = 6 WPM
    // 255 = 48 WPM

    unsigned char wpmRadioSend = round((wpm-6) * (6.071));
    //qInfo(logRig()) << "Setting keyspeed to " << wpm << "WPM, via command value" << wpmRadioSend;
    QByteArray payload;
    payload.setRawData("\x14\x0C", 2);
    payload.append(bcdEncodeInt(wpmRadioSend));
    prepDataAndSend(payload);
}

void rigCommander::getKeySpeed()
{
    QByteArray payload;
    payload.setRawData("\x14\x0C", 2);
    prepDataAndSend(payload);
}

void rigCommander::setManualNotch(bool enabled)
{
    QByteArray payload("\x16\x48");
    payload.append((unsigned char)enabled);
    prepDataAndSend(payload);
}

void rigCommander::getManualNotch()
{
    QByteArray payload;
    payload.setRawData("\x16\x48", 2);
    prepDataAndSend(payload);
}

void rigCommander::getRigID()
{
    QByteArray payload;
    payload.setRawData("\x19\x00", 2);
    prepDataAndSend(payload);
}

void rigCommander::setRigID(unsigned char rigID)
{
    // This function overrides radio model detection.
    // It can be used for radios without Rig ID commands,
    // or to force a specific radio model

    qInfo(logRig()) << "Setting rig ID to: (int)" << (int)rigID;


    lookingForRig = true;
    foundRig = false;

    // needed because this is a fake message and thus the value is uninitialized
    // this->civAddr comes from how rigCommander is setup and should be accurate.
    this->incomingCIVAddr = this->civAddr;

    this->model = determineRadioModel(rigID);
    rigCaps.modelID = rigID;
    rigCaps.model = determineRadioModel(rigID);

    determineRigCaps();
}

void rigCommander::changeLatency(const quint16 value)
{
    emit haveChangeLatency(value);
}

void rigCommander::sayAll()
{
    QByteArray payload;
    payload.setRawData("\x13\x00", 2);
    prepDataAndSend(payload);
}

void rigCommander::sayFrequency()
{
    QByteArray payload;
    payload.setRawData("\x13\x01", 2);
    prepDataAndSend(payload);
}

void rigCommander::sayMode()
{
    QByteArray payload;
    payload.setRawData("\x13\x02", 2);
    prepDataAndSend(payload);
}

// Other:

QByteArray rigCommander::stripData(const QByteArray &data, unsigned char cutPosition)
{
    QByteArray rtndata;
    if(data.length() < cutPosition)
    {
        return rtndata;
    }

    rtndata = data.right(cutPosition);
    return rtndata;
}

void rigCommander::sendState()
{
    emit stateInfo(&state);
}

void rigCommander::radioSelection(QList<radio_cap_packet> radios)
{
    emit requestRadioSelection(radios);
}

void rigCommander::radioUsage(quint8 radio, quint8 busy, QString user, QString ip) {
    emit setRadioUsage(radio, busy, user, ip);
}

void rigCommander::setCurrentRadio(quint8 radio) {
    emit selectedRadio(radio);
}

void rigCommander::stateUpdated()
{
    // A remote process has updated the rigState
    // First we need to find which item(s) have been updated and send the command(s) to the rig.

    QMap<stateTypes, value>::iterator i = state.map.begin();
    while (i != state.map.end()) {
        if (!i.value()._valid || i.value()._updated)
        {
            i.value()._updated = false;
            i.value()._valid = true; // Set value to valid as we have requested it (even if we haven't had a response)
            qDebug(logRigCtlD()) << "Got new value:" << i.key() << "=" << i.value()._value;
            switch (i.key()) {
            case VFOAFREQ:
                if (i.value()._valid) {
                    freqt freq;
                    freq.Hz = state.getInt64(VFOAFREQ);
                    setFrequency(0, freq);
                    setFrequency(0, freq);
                    setFrequency(0, freq);
                }
                getFrequency();
                break;
            case VFOBFREQ:
                if (i.value()._valid) {
                    freqt freq;
                    freq.Hz = state.getInt64(VFOBFREQ);
                    setFrequency(1, freq);
                    setFrequency(1, freq);
                    setFrequency(1, freq);
                }
                getFrequency();
                break;
            case CURRENTVFO:
                // Work on VFOB - how do we do this?
                break;
            case PTT:
                if (i.value()._valid) {
                    setPTT(state.getBool(PTT));
                    setPTT(state.getBool(PTT));
                    setPTT(state.getBool(PTT));
                }
                getPTT();
                break;
            case MODE:
            case FILTER:
                if (state.isValid(MODE) && state.isValid(FILTER)) {
                    setMode(state.getChar(MODE), state.getChar(FILTER));
                }
                getMode();
                break;
            case PASSBAND:
                if (i.value()._valid && state.isValid(MODE)) {
                    setPassband(state.getUInt16(PASSBAND));
                }
                getPassband();
                break;
            case DUPLEX:
                if (i.value()._valid) {
                    setDuplexMode(state.getDuplex(DUPLEX));
                }
                getDuplexMode();
                break;
            case DATAMODE:
                if (i.value()._valid) {
                    setDataMode(state.getBool(DATAMODE), state.getChar(FILTER));
                }
                getDataMode();
                break;
            case ANTENNA:
            case RXANTENNA:
                if (i.value()._valid) {
                    setAntenna(state.getChar(ANTENNA), state.getBool(RXANTENNA));
                }
                getAntenna();
                break;
            case ANTENNATYPE:
                if(i.value()._valid) {
                    setAntennaType(state.getChar(ANTENNATYPE));
                }
                getAntennaType();
                break;
            case CTCSS:
                if (i.value()._valid) {
                    setTone(state.getChar(CTCSS));
                }
                getTone();
                break;
            case TSQL:
                if (i.value()._valid) {
                    setTSQL(state.getChar(TSQL));
                }
                getTSQL();
                break;
            case DTCS:
                if (i.value()._valid) {
                    setDTCS(state.getChar(DTCS), false, false); // Not sure about this?
                }
                getDTCS();
                break;
            case CSQL:
                if (i.value()._valid) {
                    setTone(state.getChar(CSQL));
                }
                getTone();
                break;
            case PREAMP:
                if (i.value()._valid) {
                    setPreamp(state.getChar(PREAMP));
                }
                getPreamp();
                break;
            case ATTENUATOR:
                if (i.value()._valid) {
                    setAttenuator(state.getChar(ATTENUATOR));
                }
                getAttenuator();
                break;
            case AFGAIN:
                if (i.value()._valid) {
                    setAfGain(state.getChar(AFGAIN));
                }
                getAfGain();
                break;
            case RFGAIN:
                if (i.value()._valid) {
                    setRfGain(state.getChar(RFGAIN));
                }
                getRfGain();
                break;
            case SQUELCH:
                if (i.value()._valid) {
                    setSquelch(state.getChar(SQUELCH));
                }
                getSql();
                break;
            case RFPOWER:
                if (i.value()._valid) {
                    setTxPower(state.getChar(RFPOWER));
                }
                getTxLevel();
                break;
            case MICGAIN:
                if (i.value()._valid) {
                    setMicGain(state.getChar(MICGAIN));
                }
                getMicGain();
                break;
            case COMPLEVEL:
                if (i.value()._valid) {
                    setCompLevel(state.getChar(COMPLEVEL));
                }
                getCompLevel();
                break;
            case MONITORLEVEL:
                if (i.value()._valid) {
                    setMonitorGain(state.getChar(MONITORLEVEL));
                }
                getMonitorGain();
                break;
            case VOXGAIN:
                if (i.value()._valid) {
                    setVoxGain(state.getChar(VOXGAIN));
                }
                getVoxGain();
                break;
            case ANTIVOXGAIN:
                if (i.value()._valid) {
                    setAntiVoxGain(state.getChar(ANTIVOXGAIN));
                }
                getAntiVoxGain();
                break;
            case NBFUNC:
                if (i.value()._valid) {
                    setNB(state.getBool(NBFUNC));
                }
                getNB();
                break;
            case NRFUNC:
                if (i.value()._valid) {
                    setNR(state.getBool(NRFUNC));
                }
                getNR();
                break;
            case ANFFUNC:
                if (i.value()._valid) {
                    setAutoNotch(state.getBool(ANFFUNC));
                }
                getAutoNotch();
                break;
            case TONEFUNC:
                if (i.value()._valid) {
                    setToneEnabled(state.getBool(TONEFUNC));
                }
                getToneEnabled();
                break;
            case TSQLFUNC:
                if (i.value()._valid) {
                    setToneSql(state.getBool(TSQLFUNC));
                }
                getToneSqlEnabled();
                break;
            case COMPFUNC:
                if (i.value()._valid) {
                    setCompressor(state.getBool(COMPFUNC));
                }
                getCompressor();
                break;
            case MONFUNC:
                if (i.value()._valid) {
                    setMonitor(state.getBool(MONFUNC));
                }
                getMonitor();
                break;
            case VOXFUNC:
                if (i.value()._valid) {
                    setVox(state.getBool(VOXFUNC));
                }
                getVox();
                break;
            case SBKINFUNC:
                if (i.value()._valid) {
                    setBreakIn(state.getBool(VOXFUNC));
                }
                getVox();
                break;
            case FBKINFUNC:
                if (i.value()._valid) {
                    setBreakIn(state.getBool(VOXFUNC) << 1);
                }
                getBreakIn();
                break;
            case MNFUNC:
                if (i.value()._valid) {
                    setManualNotch(state.getBool(MNFUNC));
                }
                getManualNotch();
                break;
            case SCOPEFUNC:
                if (i.value()._valid) {
                    if (state.getBool(SCOPEFUNC)) {
                        enableSpectOutput();
                    }
                    else {
                        disableSpectOutput();
                    }
                }
                break;
            case RIGINPUT:
                if (i.value()._valid) {
                    setModInput(state.getInput(RIGINPUT), state.getBool(DATAMODE));
                }
                getModInput(state.getBool(DATAMODE));
                break;
            case POWERONOFF:
                if (i.value()._valid) {
                    if (state.getBool(POWERONOFF)) {
                        powerOn();
                    }
                    else {
                        powerOff();
                    }
                }
                break;
            case RITVALUE:
                if (i.value()._valid) {
                    setRitValue(state.getInt32(RITVALUE));
                }
                getRitValue();
                break;
             case RITFUNC:
                 if (i.value()._valid) {
                     setRitEnable(state.getBool(RITFUNC));
                 }
                 getRitEnabled();
                 break;
                 // All meters can only be updated from the rig end.
             case SMETER:
             case SWRMETER:
             case POWERMETER:
             case ALCMETER:
             case COMPMETER:
             case VOLTAGEMETER:
             case CURRENTMETER:
                 break;
             case AGC:
                 break;
             case MODINPUT:
                 break;
             case FAGCFUNC:
                 break;
             case AIPFUNC:
                 break;
             case APFFUNC:
                 break;
             case RFFUNC: // Should this set RF output power to 0?
                 break;
             case AROFUNC:
                 break;
             case MUTEFUNC:
                 if (i.value()._valid) {
                     setAfMute(state.getBool(MUTEFUNC));
                 }
                 getAfMute();
                 break;
             case VSCFUNC:
                 break;
             case REVFUNC:
                 break;
             case SQLFUNC:
                 break;
             case ABMFUNC:
                 break;
             case BCFUNC:
                 break;
             case MBCFUNC:
                 break;
             case AFCFUNC:
                 break;
             case SATMODEFUNC:
                 break;
             case NBDEPTH:
                 break;
             case NBWIDTH:
                 break;
             case NB:
                 break;
             case NR: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x06");
                     payload.append(bcdEncodeInt(state.getChar(NR)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case PBTIN: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x07");
                     payload.append(bcdEncodeInt(state.getChar(PBTIN)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case PBTOUT: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x08");
                     payload.append(bcdEncodeInt(state.getChar(PBTOUT)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case CWPITCH: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x09");
                     payload.append(bcdEncodeInt(state.getChar(CWPITCH)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case KEYSPD: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x0c");
                     payload.append(bcdEncodeInt(state.getChar(KEYSPD)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case NOTCHF: {
                 if (i.value()._valid) {
                     QByteArray payload("\x14\x0d");
                     payload.append(bcdEncodeInt(state.getChar(NOTCHF)));
                     prepDataAndSend(payload);
                 }
                 break;
             }
             case IF: {
                 if (i.value()._valid) {
                     setIFShift(state.getChar(IF));
                 }
                 getIFShift();
                 break;
             }
             case APF:
                 break;
             case BAL:
                 break;
             case RESUMEFUNC:
                 break;
             case TBURSTFUNC:
                 break;
             case TUNERFUNC:
                 if (i.value()._valid) {  
                     setATU(state.getBool(TUNERFUNC));
                 }
                 getATUStatus();
                 break;
             case LOCKFUNC:
                 if (i.value()._valid) {
                     setDialLock(state.getBool(LOCKFUNC));
                 }
                 getDialLock();
                 break;

            case ANN:
            case APO:
            case BACKLIGHT:
            case BEEP:
            case TIME:
            case BAT:
            case KEYLIGHT:
                break;

            }
        }
        ++i;
    }
}

void rigCommander::getDebug()
{
    // generic debug function for development.
    emit getMoreDebug();
}

void rigCommander::printHex(const QByteArray &pdata)
{
    printHex(pdata, false, true);
}

void rigCommander::printHex(const QByteArray &pdata, bool printVert, bool printHoriz)
{
    qDebug(logRig()) << "---- Begin hex dump -----:";
    QString sdata("DATA:  ");
    QString index("INDEX: ");
    QStringList strings;

    for(int i=0; i < pdata.length(); i++)
    {
        strings << QString("[%1]: %2").arg(i,8,10,QChar('0')).arg((unsigned char)pdata[i], 2, 16, QChar('0'));
        sdata.append(QString("%1 ").arg((unsigned char)pdata[i], 2, 16, QChar('0')) );
        index.append(QString("%1 ").arg(i, 2, 10, QChar('0')));
    }

    if(printVert)
    {
        for(int i=0; i < strings.length(); i++)
        {
            //sdata = QString(strings.at(i));
            qDebug(logRig()) << strings.at(i);
        }
    }

    if(printHoriz)
    {
        qDebug(logRig()) << index;
        qDebug(logRig()) << sdata;
    }
    qDebug(logRig()) << "----- End hex dump -----";
}

void rigCommander::dataFromServer(QByteArray data)
{
    //qInfo(logRig()) << "***************** emit dataForComm()" << data;
    emit dataForComm(data);
}

quint8* rigCommander::getGUID() {
    return guid;
}







