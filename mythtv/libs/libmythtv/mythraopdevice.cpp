#include <QTimer>
#include <QtEndian>
#include <QTcpSocket>

#include "mthread.h"
#include "mythlogging.h"
#include "mythcorecontext.h"

#include "bonjourregister.h"
#include "mythraopconnection.h"
#include "mythraopdevice.h"

MythRAOPDevice* MythRAOPDevice::gMythRAOPDevice = NULL;
MThread*        MythRAOPDevice::gMythRAOPDeviceThread = NULL;
QMutex*         MythRAOPDevice::gMythRAOPDeviceMutex = new QMutex(QMutex::Recursive);

#define LOC QString("RAOP Device: ")

bool MythRAOPDevice::Create(void)
{
    QMutexLocker locker(gMythRAOPDeviceMutex);

    // don't bother trying to start if there is no private key
    if (!MythRAOPConnection::LoadKey())
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Aborting startup - no key found.");
        return false;
    }

    // create the device thread
    if (!gMythRAOPDeviceThread)
        gMythRAOPDeviceThread = new MThread("RAOPDevice");
    if (!gMythRAOPDeviceThread)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to create RAOP device thread.");
        return false;
    }

    // create the device object
    if (!gMythRAOPDevice)
        gMythRAOPDevice = new MythRAOPDevice();
    if (!gMythRAOPDevice)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to create RAOP device object.");
        return false;
    }

    // start the thread
    if (!gMythRAOPDeviceThread->isRunning())
    {
        gMythRAOPDevice->moveToThread(gMythRAOPDeviceThread->qthread());
        QObject::connect(
            gMythRAOPDeviceThread->qthread(), SIGNAL(started()),
            gMythRAOPDevice,                  SLOT(Start()));
        gMythRAOPDeviceThread->start(QThread::LowestPriority);
    }

    LOG(VB_GENERAL, LOG_INFO, LOC + "Created RAOP device objects.");
    return true;
}

void MythRAOPDevice::Cleanup(void)
{
    LOG(VB_GENERAL, LOG_INFO, LOC + "Cleaning up.");

    if (gMythRAOPDevice)
        gMythRAOPDevice->Teardown();

    QMutexLocker locker(gMythRAOPDeviceMutex);
    if (gMythRAOPDeviceThread)
    {
        gMythRAOPDeviceThread->exit();
        gMythRAOPDeviceThread->wait();
    }
    delete gMythRAOPDeviceThread;
    gMythRAOPDeviceThread = NULL;

    delete gMythRAOPDevice;
    gMythRAOPDevice = NULL;
}

MythRAOPDevice::MythRAOPDevice()
  : ServerPool(), m_name(QString("MythTV")), m_bonjour(NULL), m_valid(false),
    m_lock(new QMutex(QMutex::Recursive)), m_setupPort(5000)
{
    for (int i = 0; i < RAOP_HARDWARE_ID_SIZE; i++)
        m_hardwareId.append((random() % 80) + 33);
}

MythRAOPDevice::~MythRAOPDevice()
{
    Teardown();

    delete m_lock;
    m_lock = NULL;
}

void MythRAOPDevice::Teardown(void)
{
    QMutexLocker locker(m_lock);

    // invalidate
    m_valid = false;

    // disconnect from mDNS
    delete m_bonjour;
    m_bonjour = NULL;

    // disconnect clients
    foreach (MythRAOPConnection* client, m_clients)
    {
        disconnect(client->GetSocket(), 0, 0, 0);
        delete client;
    }
    m_clients.clear();
}

void MythRAOPDevice::Start(void)
{
    QMutexLocker locker(m_lock);

    // already started?
    if (m_valid)
        return;

    // join the dots
    connect(this, SIGNAL(newConnection(QTcpSocket *)),
            this, SLOT(newConnection(QTcpSocket *)));

    // start listening for connections (try a few ports in case the default is in use)
    int baseport = m_setupPort;
    while (m_setupPort < baseport + RAOP_PORT_RANGE)
    {
        if (listen(gCoreContext->MythHostAddress(), m_setupPort))
        {
            LOG(VB_GENERAL, LOG_INFO, LOC +
                QString("Listening for connections on port %1").arg(m_setupPort));
            break;
        }
        m_setupPort++;
    }

    if (m_setupPort >= baseport + RAOP_PORT_RANGE)
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to find a port for incoming connections.");

    // announce service
    m_bonjour = new BonjourRegister(this);
    if (!m_bonjour)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to create Bonjour object.");
        return;
    }

    // give each frontend a unique name
    int multiple = m_setupPort - baseport;
    if (multiple > 0)
        m_name += QString::number(multiple);

    QByteArray name = m_hardwareId.toHex();
    name.append("@");
    name.append(m_name);
    name.append(" on ");
    name.append(gCoreContext->GetHostName());
    QByteArray type = "_raop._tcp";
    QByteArray txt;
    txt.append(6); txt.append("tp=UDP");
    txt.append(8); txt.append("sm=false");
    txt.append(8); txt.append("sv=false");
    txt.append(4); txt.append("ek=1");
    txt.append(6); txt.append("et=0,1");
    txt.append(6); txt.append("cn=0,1");
    txt.append(4); txt.append("ch=2");
    txt.append(5); txt.append("ss=16");
    txt.append(8); txt.append("sr=44100");
    txt.append(8); txt.append("pw=false");
    txt.append(4); txt.append("vn=3");
    txt.append(9); txt.append("txtvers=1");

    if (!m_bonjour->Register(m_setupPort, type, name, txt))
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to register service.");
        return;
    }

    m_valid = true;
    return;
}

bool MythRAOPDevice::NextInAudioQueue(MythRAOPConnection *conn)
{
    QMutexLocker locker(m_lock);
    QList<MythRAOPConnection *>::iterator it;
    for (it = m_clients.begin(); it != m_clients.end(); ++it)
        if (!(*it)->HasAudio())
            return conn == (*it);
    return true;
}

void MythRAOPDevice::newConnection(QTcpSocket *client)
{
    QMutexLocker locker(m_lock);
    LOG(VB_GENERAL, LOG_INFO, LOC + QString("New connection from %1:%2")
        .arg(client->peerAddress().toString()).arg(client->peerPort()));

    int port = 6000;
    while (port < (6000 + RAOP_PORT_RANGE))
    {
        bool found = false;
        foreach (MythRAOPConnection* client, m_clients)
        {
            if (client->GetDataPort() == port)
            {
                found = true;
                port++;
            }
        }
        if (!found)
            break;
    }

    MythRAOPConnection *obj =
            new MythRAOPConnection(this, client, m_hardwareId, port);
    if (obj->Init())
    {
        m_clients.append(obj);
        connect(client, SIGNAL(disconnected()), this, SLOT(deleteClient()));
        return;
    }

    LOG(VB_GENERAL, LOG_ERR, LOC + "Failed to initialise client connection - closing.");
    delete obj;
    client->disconnectFromHost();
    delete client;
}

void MythRAOPDevice::deleteClient(void)
{
    QMutexLocker locker(m_lock);
    QList<MythRAOPConnection *>::iterator it;
    for (it = m_clients.begin(); it != m_clients.end(); ++it)
    {
        if ((*it)->GetSocket()->state() == QTcpSocket::UnconnectedState)
        {
            LOG(VB_GENERAL, LOG_INFO, LOC + "Removing client connection.");
            delete *it;
            m_clients.removeOne(*it);
            return;
        }
    }
}
