#include "pdfobjectmodel.h"
#include "podofoutil.h"

#include <QPixmap>

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
    std::vector<PdfObjectModelNode*> GetAliases(const PdfObject * object) const;

    // Called from each node's ctor
    void NodeCreated(PdfObjectModelNode* node);
    // Called from each node's dtor
    void NodeDeleted(PdfObjectModelNode* node);

    PdfDocument* m_pDoc;
    const bool m_bFollowReferences;
    typedef std::multimap<const PdfObject*,PdfObjectModelNode*> NodeAliasMap;
    NodeAliasMap m_nodeAliases;
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
    int CountChildren() const { if (IsPretendEmpty()) return 0; EnsureChildrenLoaded(); return m_children.size(); }

    // Get the n'th child node of this object, or 0 if no such child
    // exists.
    PdfObjectModelNode* GetChild(int n) const { if (IsPretendEmpty()) return NULL; EnsureChildrenLoaded(); return n < m_children.size() ? m_children[n] : 0; }

    // Return the immediate parent of this object - a node for a reference
    // if object was referenced, otherwise the container in which the object
    // is contained.
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

    // Return a list of nodes that track the same object as this node. The alias
    // list includes this node.
    std::vector<PdfObjectModelNode*> GetAliases() const { return m_pTree->GetAliases(m_pObject); }

    // Forget about any children and re-scan for children next time anyone
    // wants to know about them. Call this method before doing something
    // to the children of this node that'll invalidate pointers to the object's
    // children.
    //
    // Note that as this invalidates any QModelIndex's for the children of this node,
    // this method should only be called by the model, which can properly inform
    // users of those indexes.
    void InvalidateChildren();

    // Reset the model's view of the tree under this node and update its view of this
    // node's data.
    void ResetSubtree();

    // Set the item's value to `data'. 
    bool SetRawData(const QByteArray& data);

    // Pretend to have no children. This is useful when resetting a subtree.
    void SetPretendEmpty(bool empty) { m_bPretendEmpty = empty; }
    // Are we pretending to have no children?
    bool IsPretendEmpty() const { return m_bPretendEmpty; }



    // Insert an element into this array node, creating a new child. The `row'
    // argument is the row number that the newly inserted row should have
    // (effectively the number of the existing row that the new row should be
    // inserted BEFORE). If the row number is not specified, a row is appended.
    void InsertElement(int row);
    bool CanInsertElement(int row) const;

    // The newly inserted child will have value `null'. `keyName' will be used
    // to set the name for the new entry. `keyName' is ignored for arrays.
    void InsertKey(const PdfName & keyName);
    bool CanInsertKey(const PdfName & keyName) const;

private:

    // Make sure the child list is populated.
    inline void EnsureChildrenLoaded() const { if (!m_bChildrenLoaded) { const_cast<PdfObjectModelNode*>(this)->PopulateChildren(); } }

    // Create nodes to fill the child list. Must NEVER be called except via EnsureChildrenLoaded().
    void PopulateChildren();

    // Add a child node for the passed object
    void AddNode(PdfObject* object, ParentageType pt, const PdfName & parentKey = PdfName::KeyNull )
    {
        m_children.push_back( new PdfObjectModelNode(m_pTree, object, this, parentKey, pt ) );
    }

    // Are we pretending to be empty?
    // TODO: merge with children loaded into flags variable for RAM usage
    bool m_bPretendEmpty;

    // True iff this object has a populated list of children.
    bool m_bChildrenLoaded;

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
}

PdfObjectModelTree::~PdfObjectModelTree()
{
    delete m_pRoot;
    assert(m_nodeAliases.size() == 0);
}

std::vector<PdfObjectModelNode*> PdfObjectModelTree::GetAliases(const PdfObject* item) const
{
    std::pair<NodeAliasMap::const_iterator,NodeAliasMap::const_iterator> r = m_nodeAliases.equal_range(item);
    std::vector<PdfObjectModelNode*> aliases;
    for ( NodeAliasMap::const_iterator it = r.first;
          it != r.second;
	  ++it)
        aliases.push_back( (*it).second );
    return aliases;
}

void PdfObjectModelTree::NodeCreated(PdfObjectModelNode* node)
{
    const PdfObject * const obj = node->GetObject();
    m_nodeAliases.insert( pair<const PdfObject*,PdfObjectModelNode*>(obj, node) );
}

void PdfObjectModelTree::NodeDeleted(PdfObjectModelNode* node)
{
    const PdfObject * const obj = node->GetObject();
    std::multimap<const PdfObject*, PdfObjectModelNode*>::iterator it = m_nodeAliases.lower_bound(obj);
    while ( (*it).first == obj && (*it).second != node )
        ++it;
    if ( (*it).first != obj || (*it).second != node )
        throw std::logic_error("Could not find object,node pair for node being deleted in alias map");
    m_nodeAliases.erase(it);
}

PdfObjectModelNode::PdfObjectModelNode(PdfObjectModelTree * tree,
                                       PdfObject* object,
                                       PdfObjectModelNode* parent,
                                       const PdfName & parentKey,
                                       ParentageType parentType)
    : m_bPretendEmpty(false),
      m_bChildrenLoaded(false),
      m_pTree(tree),
      m_pObject(object),
      m_pParent(parent),
      m_parentKey(parentKey),
      m_parentType(parentType),
      m_children()
{
    if (parentType != PT_Root && !parent)
        throw std::invalid_argument("Non-root node with null parent");

    m_pTree->NodeCreated(this);
}

bool PdfObjectModelNode::CanInsertElement(int row) const
{
    return  row > 0
            && row <= CountChildren()
            && m_pObject->IsArray();
}

void PdfObjectModelNode::InsertElement(int row)
{
    assert(CanInsertElement(row));
    PdfArray & a = m_pObject->GetArray();
    std::vector<PdfObject>::iterator it = a.begin();
    std::advance(it, row);
    a.insert(it, PdfVariant::NullValue);
}

bool PdfObjectModelNode::CanInsertKey(const PdfName& keyName) const
{
    return keyName != PdfName::KeyNull
           && m_pObject->IsDictionary()
           && !m_pObject->GetDictionary().HasKey(keyName);
}

void PdfObjectModelNode::InsertKey(const PdfName& keyName)
{
    assert(CanInsertKey(keyName));
    m_pObject->GetDictionary().AddKey(keyName, PdfVariant::NullValue);
}

void PdfObjectModelNode::InvalidateChildren()
{
    // Delete all the children of this object and flag it as needing to
    // rescan for children next time the child list is accessed.
    const std::vector<PdfObjectModelNode*>::iterator itEnd = m_children.end();
    for (std::vector<PdfObjectModelNode*>::iterator it = m_children.begin();
         it != itEnd;
         ++it)
        delete *it;

    m_children.clear();
    m_bChildrenLoaded = false;
}

PdfObjectModelNode::~PdfObjectModelNode()
{
    InvalidateChildren();
    m_pTree->NodeDeleted(this);
}

void PdfObjectModelNode::PopulateChildren()
{
    // This method must never be called except via EnsureChildrenLoaded()
    // and only by that if the child list is not populated.
    assert(!m_bChildrenLoaded);

    if (m_pTree->FollowReferences() && m_pObject->IsReference())
    {
        // We must follow the reference and create a child node under it
        PdfObject * const referee =  m_pTree->GetDocument()->GetObjects().GetObject(m_pObject->GetReference());
        if (referee)
        {
            /* CR: We dynamically construct the tree now so we don't need to do this
             *
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
                    return;
                }
                parent = parent->GetParent();
            }
            assert(m_pObject);
            */
            AddNode( referee, PT_Referenced );
        }
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

    m_bChildrenLoaded = true;
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

bool PdfObjectModelNode::SetRawData(const QByteArray & data)
{
    // Try to parse as a PdfVariant. Failure will throw an exception.
    PdfVariant variant;
    PdfTokenizer tokenizer (data.data(), data.size());
    tokenizer.GetNextVariant(variant);

    InvalidateChildren();
    *(m_pObject) = variant;
    return true;
}


}; // end anonymous namespace


PdfObjectModel::PdfObjectModel(PdfDocument* doc, QObject* parent)
    : QAbstractTableModel(parent), m_bDocChanged(false), m_pTree(0)
{
    setupModelData(doc);
}

PdfObjectModel::~PdfObjectModel()
{
    delete static_cast<PdfObjectModelTree*>(m_pTree);
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
    m_pTree = new PdfObjectModelTree(doc, catalog, true);
}

void PdfObjectModel::PrepareForSubtreeChange(const QModelIndex& index)
{
    assert(index.isValid());
    // Loop over all aliases of this node and prepare their subtrees
    // for the change to the underlying data model. Note that the alias list
    // includes the original node, so we don't  have to handle it specially.
    PdfObjectModelNode* node = static_cast<PdfObjectModelNode*>(index.internalPointer());
    const int childCount = node->CountChildren();
    const PdfObject * const obj = node->GetObject();
    if (!childCount)
        // nothing to change
        return;
    std::vector<PdfObjectModelNode*> aliases = node->GetAliases();
    std::vector<PdfObjectModelNode*>::iterator itEnd = aliases.end();
    for (std::vector<PdfObjectModelNode*>::iterator it = aliases.begin();
         it != itEnd;
         ++it)
    {
        // Inform the model about the change to this particular subtree
        // alias nodes MUST have the same number of children and same associated object.
        assert(obj == (*it)->GetObject());
        assert(childCount == (*it)->CountChildren());
        // Find out what this particular node's position within its parent node
        // is.
        beginRemoveRows(
                        createIndex( (*it)->GetIndexInParent(), 0, (*it) ),
                        0,
                        (*it)->CountChildren() - 1
                        );
        (*it)->InvalidateChildren();
        (*it)->SetPretendEmpty(true);
        endRemoveRows();
    }
}

void PdfObjectModel::SubtreeChanged(const QModelIndex& index)
{
    assert(index.isValid());
    PdfObjectModelNode* node = static_cast<PdfObjectModelNode*>(index.internalPointer());
    const PdfObject * const obj = node->GetObject();
    // Loop over all aliases of this node and inform the model the tree
    // below that node has changed.
    std::vector<PdfObjectModelNode*> aliases = node->GetAliases();
    std::vector<PdfObjectModelNode*>::iterator itEnd = aliases.end();
    for (std::vector<PdfObjectModelNode*>::iterator it = aliases.begin();
         it != itEnd;
         ++it)
    {
        // alias nodes MUST have the same associated object.
        assert(obj == (*it)->GetObject());
        // Inform the model about changes to this particular alias node
        (*it)->SetPretendEmpty(false);
        QModelIndex nodeIndex = createIndex((*it)->GetIndexInParent(), 0, (*it) );
        if ((*it)->CountChildren())
        {
            beginInsertRows(nodeIndex, 0, (*it)->CountChildren() - 1 );
            endInsertRows();
        }
        emit dataChanged( nodeIndex, nodeIndex );
    }
}

QModelIndex PdfObjectModel::index(int row, int column, const QModelIndex& parent) const
{
    if (!parent.isValid())
    {
        // We've been asked for an item in the top-level table. We currently only
        // support one-item trees (single rooted) so just return the root node.
        if (row == 0)
            return createIndex(row, column, static_cast<PdfObjectModelTree*>(m_pTree)->GetRoot());
        else
            return QModelIndex();
    }
    else
    {
        PdfObjectModelNode * parentNode = static_cast<PdfObjectModelNode*>(parent.internalPointer());
        PdfObjectModelNode * childNode = parentNode->GetChild(row);
        if (!childNode)
            return QModelIndex();
        else
            return createIndex(row, column, childNode);
    }
}

QVariant PdfObjectModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    const PdfObjectModelNode * const node = static_cast<PdfObjectModelNode*>(index.internalPointer());

    QVariant ret;

    const PdfObject* item;
    QString fileName;
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
                        {
                            // Item is a directly contained object in a dictionary, so show dictionary key
                            ret = QVariant( QString( node->GetParentKey().GetName().c_str() ) );
                        }
                        else if (item->IsArray())
                        {
                            // directly contained array element
                            ret = QVariant( QString("<element %1>").arg(node->GetIndexInParent()) );
                        }
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
                case Qt::DecorationRole:
                    item = node->GetObject();
		    switch (item->GetDataType())
		    {
			    case ePdfDataType_Bool: fileName = ":/icons/bool.png"; break;
			    case ePdfDataType_Number: fileName = ":/icons/number.png"; break;
			    case ePdfDataType_Real: fileName = ":/icons/real.png"; break;
			    case ePdfDataType_String: fileName = ":/icons/litstring.png"; break;
			    case ePdfDataType_HexString: fileName = ":/icons/hexstring.png"; break;
			    case ePdfDataType_Name: fileName = ":/icons/name.png"; break;
			    case ePdfDataType_Array: fileName = ":/icons/dictionary.png"; break;
			    case ePdfDataType_Dictionary: fileName = ":/icons/dictionary.png"; break;
			    case ePdfDataType_Null: fileName = ":/icons/empty.png"; break;
			    case ePdfDataType_Reference:
                                 fileName = node->CountChildren() ? ":/icons/reference.png" : ":/icons/dangling_reference.png";
                                 break;
			    case ePdfDataType_RawData: fileName = ""; break;
		    }
                    ret = QVariant( QPixmap(fileName) );
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
    if (!index.isValid())
        return Qt::ItemIsEnabled;

    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == 2)
        f = f | Qt::ItemIsEditable;

    return f;
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
        abort();
        return QModelIndex();
    }

    PdfObjectModelNode * const child = static_cast<PdfObjectModelNode*>(index.internalPointer());
    if (!child)
        abort();

    assert(child);
    PdfObjectModelNode * const parent = child->GetParent();
    if (!parent)
    {
        if (child != static_cast<PdfObjectModelTree*>(m_pTree)->GetRoot())
            throw std::logic_error("node with no parent not the root node");
        else
            return QModelIndex();
    }

    int parentRow = parent->GetIndexInParent();
    assert(parentRow >= 0);
    
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

    return static_cast<PdfObjectModelNode*>(index.internalPointer())->GetObject();
}

bool PdfObjectModel::setData ( const QModelIndex & index, const QVariant & value, int role )
{
    if (!index.isValid() || index.column() != 2)
        return false;
    if (value.isNull() || !value.isValid() || !value.canConvert<QByteArray>())
        return false;

    QByteArray data = value.toByteArray();
    if (data.size() == 0)
        return false;

    PdfObjectModelNode* node = static_cast<PdfObjectModelNode*>(index.internalPointer());
    const PdfObject* obj = node->GetObject();

    // Container objects need to inform the model implementation that
    // the tree structure may change after they're edited. To do this we
    // clear all rows that're children of the modified container, trim that
    // part of the model tree, then let them be re-read on demand.
    // Since a simple object might be turned into a container by the user, 
    // we do this even for simple object edits.
    PrepareForSubtreeChange(index);
    bool changed = false;
    try
    {
        changed = node->SetRawData(data);
    }
    catch (PdfError & eCode)
    {
         podofoError(eCode);
    }
    SubtreeChanged(index);
    m_bDocChanged = m_bDocChanged|changed;
    return changed;
}

bool PdfObjectModel::insertElement( int row, const QModelIndex & parent )
{
    PdfObjectModelNode * node;
    if (parent.isValid())
        node = static_cast<PdfObjectModelTree*>(m_pTree)->GetRoot();
    else
        node = static_cast<PdfObjectModelNode*>(parent.internalPointer());
    if (node->CanInsertElement(row))
    {
       beginInsertRows(parent, row, row);
       node->InsertElement(row);
       endInsertRows();
       return true;
    }
    return false;
}

bool PdfObjectModel::insertKey(const PdfName& keyName, const QModelIndex & parent )
{
    PdfObjectModelNode * node;
    if (parent.isValid())
        node = static_cast<PdfObjectModelTree*>(m_pTree)->GetRoot();
    else
        node = static_cast<PdfObjectModelNode*>(parent.internalPointer());
    if (node->CanInsertKey(keyName))
    {
        PrepareForSubtreeChange(parent);
        node->InsertKey(keyName);
        SubtreeChanged(parent);
        return true;
    }
    return false;
}

void PdfObjectModel::InvalidateChildren(const QModelIndex & index)
{
    if (index.isValid())
    {
        PdfObjectModelNode * const node = static_cast<PdfObjectModelNode*>(index.internalPointer());
        assert(node);
        emit layoutAboutToBeChanged(); // Do we really need this?
        node->InvalidateChildren();
        emit layoutChanged(); // or this?
        emit dataChanged(index, index);
    }
}

bool PdfObjectModel::IndexIsDictionary(const QModelIndex & index) const
{
    if (!index.isValid()) return false;
    return static_cast<PdfObjectModelNode*>(index.internalPointer())->GetObject()->IsDictionary();
}

bool PdfObjectModel::IndexIsArray(const QModelIndex & index) const
{
    if (!index.isValid()) return false;
    return static_cast<PdfObjectModelNode*>(index.internalPointer())->GetObject()->IsArray();
}

bool PdfObjectModel::IndexIsReference(const QModelIndex & index) const
{
    if (!index.isValid()) return false;
    return static_cast<PdfObjectModelNode*>(index.internalPointer())->GetObject()->IsReference();
}

int PdfObjectModel::IndexChildCount(const QModelIndex & index) const
{
    if (!index.isValid()) return -1;
    return static_cast<PdfObjectModelNode*>(index.internalPointer())->CountChildren();
}

bool PdfObjectModel::insertRow(int,const QModelIndex&)
{
    throw std::logic_error("Use insertElement(...) or insertKey(...) instead!");
}