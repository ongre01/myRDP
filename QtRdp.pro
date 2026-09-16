TEMPLATE = subdirs
CONFIG += ordered

client.file = RDPClient/RDPClient.pro
server.file = RDPServer/RDPServer.pro

SUBDIRS += \
    client \
    server
