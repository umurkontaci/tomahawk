/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2013, Teo Mrnjavac <teo@kde.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CredentialsManager.h"

#include "utils/Logger.h"

#include <qtkeychain/keychain.h>

#include <qjson/serializer.h>
#include <qjson/parser.h>

#include <QStringList>


namespace Tomahawk
{

namespace Accounts
{

CredentialsStorageKey::CredentialsStorageKey( const QString& service, const QString& key )
    : m_service( service )
    , m_key( key )
{}

bool
CredentialsStorageKey::operator ==( const CredentialsStorageKey& other ) const
{
    return ( m_key == other.m_key ) && ( m_service == other.m_service );
}


bool
CredentialsStorageKey::operator !=( const CredentialsStorageKey& other ) const
{
    return !operator ==( other );
}

uint
qHash( const Tomahawk::Accounts::CredentialsStorageKey& key )
{
    return qHash( key.service() + key.key() );
}


// CredentialsManager

CredentialsManager::CredentialsManager( QObject* parent )
    : QObject( parent )
{
    tDebug() << Q_FUNC_INFO;
}


void
CredentialsManager::addService( const QString& service , const QStringList& accountIds )
{
    if ( m_services.contains( service ) )
        m_services.remove( service );
    m_services.insert( service, accountIds );
    loadCredentials( service );
}


void
CredentialsManager::loadCredentials( const QString &service )
{

    const QStringList& accountIds = m_services.value( service );
    tDebug() << Q_FUNC_INFO << "keys for service" << service << ":" << accountIds;
    foreach ( QString key, accountIds )
    {
        QKeychain::ReadPasswordJob* j = new QKeychain::ReadPasswordJob( service, this );
        j->setKey( key );
        j->setAutoDelete( false );
#if defined( Q_OS_UNIX ) && !defined( Q_OS_MAC )
        j->setInsecureFallback( true );
#endif
        connect( j, SIGNAL( finished( QKeychain::Job* ) ),
                    SLOT( keychainJobFinished( QKeychain::Job* ) ) );
        m_readJobs[ service ] << j;
        j->start();
        tDebug()  << "Launching QtKeychain readJob for" << key;
    }

    if ( m_readJobs[ service ].isEmpty() )
    {
        // We did not launch any readJob, so we're done already.
        emit serviceReady( service );
    }

}


QStringList
CredentialsManager::keys( const QString& service ) const
{
    QStringList keys;
    foreach ( const CredentialsStorageKey& k, m_credentials.keys() )
    {
        if ( k.service() == service )
            keys << k.key();
    }
    return keys;
}


QStringList
CredentialsManager::services() const
{
    return m_services.keys();
}


QVariant
CredentialsManager::credentials( const CredentialsStorageKey& key ) const
{
    return m_credentials.value( key );
}


QVariant
CredentialsManager::credentials( const QString& serviceName, const QString& key ) const
{
    return credentials( CredentialsStorageKey( serviceName, key ) );
}


void
CredentialsManager::setCredentials( const CredentialsStorageKey& csKey, const QVariant& value, bool tryToWriteAsString )
{
    QMutexLocker locker( &m_mutex );

    QKeychain::Job* j;
    if ( value.isNull() ||
         ( value.type() == QVariant::Hash && value.toHash().isEmpty() ) ||
         ( value.type() == QVariant::String && value.toString().isEmpty() ) )
    {
        if ( !m_credentials.contains( csKey ) ) //if we don't have any credentials for this key, we bail
            return;

        m_credentials.remove( csKey );

        QKeychain::DeletePasswordJob* dj = new QKeychain::DeletePasswordJob( csKey.service(), this );
        dj->setKey( csKey.key() );
        j = dj;
    }
    else
    {
        if ( value == m_credentials.value( csKey ) ) //if the credentials haven't actually changed, we bail
            return;

        m_credentials.insert( csKey, value );

        QKeychain::WritePasswordJob* wj = new QKeychain::WritePasswordJob( csKey.service(), this );
        wj->setKey( csKey.key() );

        Q_ASSERT( value.type() == QVariant::String || value.type() == QVariant::Hash );

        if ( tryToWriteAsString && value.type() == QVariant::String )
        {
            wj->setTextData( value.toString() );
        }
        else if ( value.type() == QVariant::Hash )
        {
            QJson::Serializer serializer;
            bool ok;
            QByteArray data = serializer.serialize( value.toHash(), &ok );

            if ( ok )
            {
                tDebug() << "About to write credentials for key" << csKey.key();
            }
            else
            {
                tDebug() << "Cannot serialize credentials for writing" << csKey.key();
            }

            wj->setTextData( data );
        }

        j = wj;
    }

    j->setAutoDelete( false );
#if defined( Q_OS_UNIX ) && !defined( Q_OS_MAC )
    j->setInsecureFallback( true );
#endif
    connect( j, SIGNAL( finished( QKeychain::Job* ) ),
             SLOT( keychainJobFinished( QKeychain::Job* ) ) );
    j->start();
}


void
CredentialsManager::setCredentials( const QString& serviceName, const QString& key, const QVariantHash& value )
{
    setCredentials( CredentialsStorageKey( serviceName, key ), QVariant( value ) );
}


void
CredentialsManager::setCredentials( const QString& serviceName, const QString& key, const QString& value )
{
    setCredentials( CredentialsStorageKey( serviceName, key ), QVariant( value ), true );
}


void
CredentialsManager::keychainJobFinished( QKeychain::Job* j )
{
    tDebug() << Q_FUNC_INFO;
    if ( QKeychain::ReadPasswordJob* readJob = qobject_cast< QKeychain::ReadPasswordJob* >( j ) )
    {
        if ( readJob->error() == QKeychain::NoError )
        {
            tDebug() << "QtKeychain readJob for" << readJob->service() << "/"
                     << readJob->key() << "finished without errors";

            QVariant creds;
            QJson::Parser parser;
            bool ok;

            creds = parser.parse( readJob->textData().toLatin1(), &ok );

            QVariantMap map = creds.toMap();
            QVariantHash hash;
            for ( QVariantMap::const_iterator it = map.constBegin();
                  it != map.constEnd(); ++it )
            {
                hash.insert( it.key(), it.value() );
            }
            creds = QVariant( hash );

            if ( !ok || creds.toHash().isEmpty() )
            {
                creds = QVariant( readJob->textData() );
            }

            m_credentials.insert( CredentialsStorageKey( readJob->service(), readJob->key() ), creds );
        }
        else
        {
            tDebug() << "QtKeychain readJob for" << readJob->service() << "/" << readJob->key() << "finished with error:" << j->error() << j->errorString();
        }

        m_readJobs[ readJob->service() ].removeOne( readJob );

        if ( m_readJobs[ readJob->service() ].isEmpty() )
        {
            emit serviceReady( readJob->service() );
        }
    }
    else if ( QKeychain::WritePasswordJob* writeJob = qobject_cast< QKeychain::WritePasswordJob* >( j ) )
    {
        tLog() << Q_FUNC_INFO << "QtKeychain writeJob for" << writeJob->service() << "/" << writeJob->key() << "finished"
               << ( ( j->error() == QKeychain::NoError ) ? "without error" : j->errorString() );
    }
    else if ( QKeychain::DeletePasswordJob* deleteJob = qobject_cast< QKeychain::DeletePasswordJob* >( j ) )
    {
        tLog() << Q_FUNC_INFO << "QtKeychain deleteJob for" << deleteJob->service() << "/" << deleteJob->key() << "finished"
               << ( ( j->error() == QKeychain::NoError ) ? "without error" : j->errorString() );
    }
    j->deleteLater();
}

} //namespace Accounts

} //namespace Tomahawk
