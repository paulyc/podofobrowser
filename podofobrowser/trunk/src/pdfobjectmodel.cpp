#include "pdfobjectmodel.h"

#include <utility>
using std::pair;
#include <vector>
#include <cassert>
#include <exception>
#include <stdexcept>

#include <podofo/podofo.h>
using namespace PoDoFo;

namespace {

class PdfObjectModelNode;

// PdfObjectModelTree keeps track of the nodes associated with a particular
// document and contains some tree-wide shared data. It also knows the root 
// of the tree of nodes for the model.
//
// The tree relies totally on the document not being modified except through
// this model tree. Direct modification of the document is likely to result in
// crashes and/or bizarre behaviour.
//
class PdfObjectModelTree
{
public:
    PdfObjectModelTree(PdfDocument * doc, PdfObject* root, bool followReferences);

    ~PdfObjectModelTree();

    PdfObjectModelNode * GetRoot() const { return m_pRoot; }

    PdfDocument* GetDocument() const { return m_pDoc; }

    bool FollowReferences() const { return m_bFollowReferences; }
private:
    friend class PdfObjectModelNode;
    // XXX TODO Force full model creation for these calls to produce correct results
    int CountAliases(const PdfObject * object) const { return m_nodeAliases.count(object); }
    std::vector<PdfObjectModelNode*> GetAliases(const PdfObject * object) const; //XXX

    // Called from each node's ctor
    void NodeCreated(PdfObjectModelNode* node);

    PdfDocument* m_pDoc;
    const bool m_bFollowReferences;
    std::multimap<const PdfObject*,PdfObjectModelNode*> m_nodeAliases;
    PdfObjectModelNode * m_pRoot;

};

// PdfObjectModelNode wraps a PdfObject item. It keeps track of the
// item's container (if a direct object) or referencer (if an indirect
// object). More than one PdfObjectModelNode can exist for a given indirect
// PdfObject, since the object may by linked to from multiple places.
//
// A PdfObjectModelNode also provides helper routines to better
// match the PdfObject interface to the interface needed by
// the Qt model design.
//
class PdfObjectModelNode
{
public:
    enum ParentageType
    {
        // Object is contained by another directly
        PT_Contained,
        // Object is referenced by another
        PT_Referenced,
        // Object (which MUST be indirect) is treated as the root
        // of the document tree and has no parent
        PT_Root
    };

    PdfObjectModelNode(PdfObjectModelTree * tree,
                       PdfObject* object,
                       PdfObjectModelNode* parent,
                       const PdfName & parentKey,
                       ParentageType parentType);

    ~PdfObjectModelNode();

    // Return the value PdfObject tracked by this node.
    // Do not delete the returned object.
    const PdfObject * GetObject() const { return m_pObject; }

    // Return the appropriate data for the node given a particular role
    QVariant GetData(int role) const;

    // Return the number of children of this node
    int CountChildren() const { return m_children.size(); }

    // Get the n'th child node of this object, or 0 if no such child
    // exists.
    PdfObjectModelNode* GetChild(int n) const { return n < m_children.size() ? m_children[n] : 0; }

    // Return the container that contains/reference this object
    PdfObjectModelNode * GetContainer() const;

    // Return the direct parent of this object - a node for a reference
    // if object was referenced, otherwise a same as GetContainer();
    PdfObjectModelNode * GetParent() const { return m_pParent; }

    // Return the index of this object inside its parent's
    // child list.
    int GetIndexInParent() const;

    // Return the key in the parent object that has this object
    // or a reference to it as its value. A null value is returned
    // if the parent is an array.
    const PdfName & GetParentKey() const { return m_parentKey; }

    // Return the number of aliases this node has, ie the number of other
    // nodes that track the same PdfObject
    int CountAliases() { return m_pTree->CountAliases(m_pObject); }

    // Return a list of nodes that track the same object as this node
    std::vector<PdfObjectModelNode*> GetAliases() { return m_pTree->GetAliases(m_pObject); }

private:
    // Create nodes to fill the child list
    void PopulateChildren();

    // Add a child node for the passed object
    void AddNode(PdfObject* object, ParentageType pt, const PdfName & parentKey = PdfName::KeyNull )
    {
        m_children.push_back( new PdfObjectModelNode(m_pTree, object, this, parentKey, pt ) );
    }

    // Tree object for this node
    PdfObjectModelTree * m_pTree;

    // Object tracked by this node
    PdfObject* m_pObject;

    ParentageType m_parentType;

    // Parent node. The meaning of this pointer varies depending on the parentage
    // relationship:
    //
    // Root: has no parent
    // Contained: parent is the node for the containing dictionary / array
    // Referenced: parent is the node for the reference object in the parent.
    //             Its parent will be the container for this object.
    PdfObjectModelNode * m_pParent;

    // Key under which item appears in containing dictionary. Empty for arrays
    // and referenced objects.
    PdfName m_parentKey;

    // A list of pointers to all children of this node
    std::vector<PdfObjectModelNode*> m_children;

};

PdfObjectModelTree::PdfObjectModelTree(PdfDocument * doc, PdfObject* root, bool followReferences)
    : m_pDoc(doc),
      m_bFollowReferences(followReferences),
      m_nodeAliases(),
      m_pRoot( new PdfObjectModelNode(this, root, NULL, PdfName::KeyNull, PdfObjectModelNode::PT_Root) )
{
    qDebug("Done creating model tree"); //XXX
}

PdfObjectModelTree::~PdfObjectModelTree()
{
    delete m_pRoot;
}

void PdfObjectModelTree::NodeCreated(PdfObjectModelNode* node)
{
    const PdfObject * const obj = node->GetObject();
    m_nodeAliases.insert( pair<const PdfObject*,PdfObjectModelNode*>(obj, node) );
}

PdfObjectModelNode::PdfObjectModelNode(PdfObjectModelTree * tree,
                                       PdfObject* object,
                                       PdfObjectModelNode* parent,
                                       const PdfName & parentKey,
                                       ParentageType parentType)
    : m_pTree(tree),
      m_pObject(object),
      m_pParent(parent),
      m_parentKey(parentKey),
      m_parentType(parentType),
      m_children()
{
    if (parentType != PT_Root && !parent)
        throw std::invalid_argument("Non-root node with null parent");

    m_pTree->NodeCreated(this);

    // todo: deferred child tree building
    PopulateChildren();
}

PdfObjectModelNode::~PdfObjectModelNode()
{
    // Clean up children
    const std::vector<PdfObjectModelNode*>::iterator itEnd = m_children.end();
    for (std::vector<PdfObjectModelNode*>::iterator it = m_children.begin();
         it != itEnd;
         ++it)
        delete *it;
}

void PdfObjectModelNode::PopulateChildren()
{
    if (m_pTree->FollowReferences() && m_pObject->IsReference())
    {
       // We must follow the reference and create a child node under it
       PdfObject * const referee =  m_pTree->GetDocument()->GetObjects().GetObject(m_pObject->GetReference());

       // Recurse up our direct heritage to see if this object number has been referenced before.
       // If it has we're in a reference cycle and should just stop here. TODO: handle cycles
       // gracefully (hyperlink?)
       const PdfReference ref = m_pObject->GetReference();
       PdfObjectModelNode * parent = m_pParent;
       while (parent)
       {
           const PdfObject* const obj = parent->GetObject();
           if (obj->IsReference() && obj->GetReference() == ref)
           {
               //qDebug("Reference cycle detected on %s, breaking recursion", ref.ToString().c_str() );
               return;
           }
           parent = parent->GetParent();
       }

       // TODO: handle dangling references
       assert(m_pObject);
       AddNode( referee, PT_Referenced );
    }
    else if (m_pObject->IsDictionary())
    {
        TKeyMap& keys ( m_pObject->GetDictionary().GetKeys() );
        for (TKeyMap::iterator it = keys.begin();
             it != keys.end();
             ++it )
        {
            AddNode( (*it).second, PT_Contained, (*it).first );
        }
    }
    else if (m_pObject->IsArray())
    {
        for (std::vector<PdfObject>::iterator it = m_pObject->GetArray().begin();
             it != m_pObject->GetArray().end();
             ++it)
        {
            AddNode( &(*it), PT_Contained );
        }
    }
}

int PdfObjectModelNode::GetIndexInParent() const
{
    if (!m_pParent)
    {
        // We don't have a parent, ie we're in the top level table. Currently
        // we only support one-entry top level tables, so we're the root element
        // at row 0.
        assert(this == m_pTree->GetRoot());
        return 0;
    }

    // find our index in the parent's child vector
    int counter = 0;
    std::vector<PdfObjectModelNode*>::iterator itEnd ( m_pParent->m_children.end() );
    for ( std::vector<PdfObjectModelNode*>::iterator it =m_pParent->m_children.begin();
          it != itEnd;
          ++it, ++counter )
    {
        if (*it == this)
            return counter;
    }
    throw std::logic_error("Node not present in parent's list of children!");
}

PdfObjectModelNode * PdfObjectModelNode::GetContainer() const
{
    PdfObjectModelNode * ret = 0;
    switch (m_parentType)
    {
        case PT_Root:
            break;
        case PT_Contained:
            ret = m_pParent;
            break;
        case PT_Referenced:
            ret = m_pParent->m_pParent;
            break;
    }
    return ret;
}

}; // end anon namespace


PdfObjectModel::PdfObjectModel(PdfDocument* doc, QObject* parent)
    : QAbstractTableModel(parent), m_pTree(0)
{
    setupModelData(doc);
}

PdfObjectModel::~PdfObjectModel()
{
    delete m_pTree;
}

void PdfObjectModel::setupModelData(PdfDocument * doc)
{
    // Find the document catalog dictionary, which we'll use as the root
    // of the tree
    const PdfObject * const trailer = doc->GetTrailer();
    if (!trailer->IsDictionary())
        throw std::invalid_argument("Document invalid - non-dictionary trailer!");

    const PdfName KeyRoot("Root");
    if (!trailer->GetDictionary().HasKey( KeyRoot) )
    {
        throw std::invalid_argument("passed document lacks catalog dictionary");
    }

    PdfObject * const catalogRef = const_cast<PdfObject*>(trailer->GetDictionary().GetKey( KeyRoot ));
    if (!catalogRef || !catalogRef->IsReference())
        throw std::invalid_argument("Invalid /Root trailer entry");

    PdfObject * const catalog = doc->GetObjects().GetObject(catalogRef->GetReference());
    if (!catalog || !catalog->IsDictionary())
        throw std::invalid_argument("Invalid or non-dictionary referenced by /Root trailer entry");

    // Create a new tree rooted on document catalog with reference following
    // turned on
    qDebug("Creating model tree..."); //XXX
    m_pTree = new PdfObjectModelTree(doc, catalog, true);
    qDebug("Done"); //XXX
}

QModelIndex PdfObjectModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!parent.isValid())
    {
        // We've been asked for an item in the top-level table. We currently only
        // support one-item trees (single rooted) so just return the root node.
        if (row == 0)
        {
            qDebug("::index asked for root node: (%i %i %p)", row, column, m_pTree->GetRoot() );
            return createIndex(row, column, m_pTree->GetRoot());
        }
        else
        {
            qDebug("::index asked for non-row-0 root node row %i **BAD**", row);
            return QModelIndex();
        }
    }
    else
    {
        PdfObjectModelNode * parentNode = static_cast<PdfObjectModelNode*>(parent.internalPointer());
        PdfObjectModelNode * childNode = parentNode->GetChild(row);
        if (!childNode)
        {
            qDebug("::index getting index for %i %i with parent %p: not found", row, column, parentNode);
            return QModelIndex();
        }
        else
        {
            qDebug("::index getting index for %i %i with parent %p: (%i %i %p)", row, column, parentNode, row, column, childNode);
            return createIndex(row, column, childNode);
        }
    }
}

QVariant PdfObjectModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    const PdfObjectModelNode * const node = static_cast<PdfObjectModelNode*>(index.internalPointer());

    QVariant ret;

    const PdfObject* item;
    const PdfParserObject* pobj;
    switch (index.column())
    {
        case 0:
            switch (role)
            {
                case Qt::DisplayRole:
                    if (!node->GetParent())
                        // root object
                        ret = QVariant( QString("/Root") );
                    else
                    {
                        item = node->GetParent()->GetObject();

                        if (item->IsDictionary())
                            // Item is a directly contained object in a dictionary, so show dictionary key
                            ret = QVariant( QString( node->GetParentKey().GetName().c_str() ) );
                        else if (item->IsArray())
                            // directly contained array element
                            ret = QVariant( QString("<element>") );
                        else if (item->IsReference())
                        {
                            // item is an indirect object from a followed reference
                            const PdfReference& ref ( item->GetReference() );
                            ret = QVariant( QString("%1 %2 obj").arg(ref.ObjectNumber()).arg(ref.GenerationNumber()) );
                        }
                        else
                            ret = QVariant( QString("<UNKNOWN>") );
                    }
                    break;
                default:
                    break;
            }
            break;
        
        case 1:
            switch (role)
            {
                case Qt::DisplayRole:
                    ret = QVariant( QString( node->GetObject()->GetDataTypeString() ) );
                    break;
                default:
                    ret = QVariant();
                    break;
            }
            break;

        case 2:
            switch (role)
            {
                case Qt::DisplayRole:
                    item = node->GetObject();

                    if (item->IsDictionary())
                    {
                        QString value("<< ");
                        if (item->GetDictionary().HasKey( PdfName::KeyType ) )
                        {
                            std::string s;
                            item->GetDictionary().GetKey( PdfName::KeyType )->ToString(s);
                            value += QString("/Type %1 ").arg(s.c_str());
                        }
                        if (item->GetDictionary().HasKey( PdfName("SubType") ) )
                        {
                            std::string s;
                            item->GetDictionary().GetKey( PdfName("SubType") )->ToString(s);
                            value += QString("/SubType %1 ").arg(s.c_str());
                        }
                        if (item->GetDictionary().HasKey( PdfName("Name") ) )
                        {
                            std::string s;
                            item->GetDictionary().GetKey( PdfName("Name") )->ToString(s);
                            value += QString("/Name %1 ").arg(s.c_str());
                        }
                        ret = QVariant( value += "... >>" );
                    }
                    else if (item->IsArray())
                    {
                        // Do nothing, since we return QVariant()
                    }
                    else
                    {
                        std::string s;
                        item->ToString(s);
                        ret = QVariant( QString( s.c_str() ) );
                    }
                    break;
                default:
                    break;
            }
            break;
    }
    return ret;
}

Qt::ItemFlags PdfObjectModel::flags(const QModelIndex &index) const
{
    // XXX TODO currently all claimed read only
    if (!index.isValid())
        return Qt::ItemIsEnabled;

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant PdfObjectModel::headerData(int section, Qt::Orientation orientation,
                    int role) const
{
     if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
     switch (section)
     {
         case 0:
             return QVariant("Object");
         case 1:
             return QVariant("Type");
         case 2:
             return QVariant("Value");
         default:
             return QVariant();
     }

     return QVariant();
}

// Find the parent of the object pointed to by index's internal pointer
// and return an index for it
QModelIndex PdfObjectModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
    {
        qDebug("::parent **CALLED WITH INVALID INDEX**");
        abort();
        return QModelIndex();
    }

    PdfObjectModelNode * const child = static_cast<PdfObjectModelNode*>(index.internalPointer());
    if (!child)
    {
        qDebug("::parent **BAD CHILD**");
        abort();
    }

    assert(child);
    PdfObjectModelNode * const parent = child->GetParent();
    if (!parent)
    {
        if (child != m_pTree->GetRoot())
        {
            qDebug("::parent(%p): Child %p has null parent but is not root node (%p)", child, m_pTree->GetRoot());
            //throw std::logic_error("node with no parent not the root node");
            abort();
        }
        else
        {
            qDebug("::parent(%p): Child is root node %p, returning invalid index", child, m_pTree->GetRoot());
            return QModelIndex();
        }
    }

    int parentRow = parent->GetIndexInParent();
    assert(parentRow >= 0);
    
    qDebug("::parent(%p): returning (%i 0 %p)", child, parentRow, parent);
    return createIndex(parentRow, 0, parent);
}

int PdfObjectModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
    {
        // Want a row count on the top level. We only support one element, so:
        return 1;
    }
    else
    {
        PdfObjectModelNode * parentNode = static_cast<PdfObjectModelNode*>(parent.internalPointer());
        return parentNode->CountChildren();
    }
}

int PdfObjectModel::columnCount(const QModelIndex &parent) const
{
    //if (!parent.isValid())
    //    return 0;
    return 3;
}

const PdfObject* PdfObjectModel::GetObjectForIndex(const QModelIndex & index) const
{
    if (!index.isValid())
        return NULL;

    PdfObjectModelNode * node = static_cast<PdfObjectModelNode*>(index.internalPointer());
    return node->GetObject();
}
