/*
    SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "datacontroloffer_v1.h"
#include "datacontroldevice_v1.h"
#include "datacontrolsource_v1.h"
// Qt
#include <QPointer>
#include <QStringList>
// Wayland
#include <qwayland-server-ext-data-control-v1.h>
// system
#include <unistd.h>

namespace KWin
{
class DataControlOfferV1InterfacePrivate : public QtWaylandServer::ext_data_control_offer_v1
{
public:
    DataControlOfferV1InterfacePrivate(DataControlOfferV1Interface *q, AbstractDataSource *source, wl_resource *resource);

    DataControlOfferV1Interface *q;
    QPointer<AbstractDataSource> source;

protected:
    void ext_data_control_offer_v1_receive(Resource *resource, const QString &mime_type, int32_t fd) override;
    void ext_data_control_offer_v1_destroy(Resource *resource) override;
    void ext_data_control_offer_v1_destroy_resource(Resource *resource) override;
};

DataControlOfferV1InterfacePrivate::DataControlOfferV1InterfacePrivate(DataControlOfferV1Interface *_q, AbstractDataSource *source, wl_resource *resource)
    : QtWaylandServer::ext_data_control_offer_v1(resource)
    , q(_q)
    , source(source)
{
}

void DataControlOfferV1InterfacePrivate::ext_data_control_offer_v1_destroy(QtWaylandServer::ext_data_control_offer_v1::Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void DataControlOfferV1InterfacePrivate::ext_data_control_offer_v1_destroy_resource(QtWaylandServer::ext_data_control_offer_v1::Resource *resource)
{
    delete q;
}

void DataControlOfferV1InterfacePrivate::ext_data_control_offer_v1_receive(Resource *resource, const QString &mimeType, qint32 fd)
{
    if (!source) {
        close(fd);
        return;
    }
    source->requestData(mimeType, fd);
}

DataControlOfferV1Interface::DataControlOfferV1Interface(AbstractDataSource *source, wl_resource *resource)
    : QObject()
    , d(new DataControlOfferV1InterfacePrivate(this, source, resource))
{
    Q_ASSERT(source);
    connect(source, &AbstractDataSource::mimeTypeOffered, this, [this](const QString &mimeType) {
        d->send_offer(mimeType);
    });
}

DataControlOfferV1Interface::~DataControlOfferV1Interface() = default;

void DataControlOfferV1Interface::sendAllOffers()
{
    Q_ASSERT(d->source);
    for (const QString &mimeType : d->source->mimeTypes()) {
        d->send_offer(mimeType);
    }
}

wl_resource *DataControlOfferV1Interface::resource() const
{
    return d->resource()->handle;
}

}

#include "moc_datacontroloffer_v1.cpp"
