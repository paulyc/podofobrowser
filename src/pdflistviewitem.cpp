/***************************************************************************
 *   Copyright (C) 2005 by Dominik Seichter                                *
 *   domseichter@web.de                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "pdflistviewitem.h"

#include <podofo.h>

#include <string>

PdfListViewItem::PdfListViewItem( QListView* parent, PoDoFo::PdfObject* object )
    : QListViewItem( parent, parent->lastItem() ), m_pObject( object ), m_bInitialized( false )
{
    m_sText = QString( "%1 %2 R  " ).arg( m_pObject->ObjectNumber() ).arg( m_pObject->GenerationNumber() );
    m_sType = "";
    setText( 0, m_sText + m_sType );
}

PdfListViewItem::PdfListViewItem( QListViewItem* parent, PoDoFo::PdfObject* object, const QString & key )
    : QListViewItem( parent ), m_pObject( object ), m_bInitialized( false )
{
    m_sText = QString( "%1 %2 R  " ).arg( m_pObject->ObjectNumber() ).arg( m_pObject->GenerationNumber() );
    m_sType = key;
    setText( 0, m_sText + m_sType );
}
 
PdfListViewItem::~PdfListViewItem()
{
}

void PdfListViewItem::init()
{
    PoDoFo::PdfError     eCode;
    PoDoFo::TCIObjKeyMap itObjs;
    PoDoFo::PdfVariant   var;
    PdfListViewItem*     child;
    std::string          str;

    if( m_bInitialized ) 
        return;

    if( m_sType.isEmpty() && m_pObject->HasKey( PoDoFo::PdfName::KeyType ) )
    {
        eCode = m_pObject->GetKeyValueVariant( PoDoFo::PdfName::KeyType, var );
        if( !eCode.IsError() && !(eCode = var.ToString( str )).IsError() )
            m_sType = str;
    }

    if( eCode.IsError() )
        eCode.PrintErrorMsg();

    setText( 0, m_sText + m_sType );

    if( m_pObject->GetObjectKeys().size() )
    {
        this->setOpen( true );

        itObjs = m_pObject->GetObjectKeys().begin();
        while( itObjs != m_pObject->GetObjectKeys().end() )
        {
            child = new PdfListViewItem( this, (*itObjs).second, QString( (*itObjs).first.Name().c_str() ) );
            child->init();
            ++itObjs;
        }
    }

    m_bInitialized = true;
}

int PdfListViewItem::compare( QListViewItem* i, int col, bool ascending ) const
{
    PdfListViewItem* item = dynamic_cast<PdfListViewItem*>(i);

    if( col || !item )
        return QListViewItem::compare( i, col, ascending );
    else
    {
        if( item->object()->ObjectNumber() == this->object()->ObjectNumber() )
            return 0;

        if( this->object()->ObjectNumber() > item->object()->ObjectNumber() )
            return ascending ? 1 : -1;
        else 
            return ascending ? -1 : 1;
    }
}
