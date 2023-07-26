#include <string>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/header_page.h"

namespace bustub {
INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, BufferPoolManager *buffer_pool_manager, const KeyComparator &comparator,
                          int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      root_page_id_(INVALID_PAGE_ID),
      buffer_pool_manager_(buffer_pool_manager),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
        // LOG_INFO("BPlusTree: leaf_max_size_=%d, internal_max_size_=%d",leaf_max_size_,internal_max_size_);
      }

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool { return root_page_id_==INVALID_PAGE_ID; }
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result, Transaction *transaction) -> bool {
  LOG_INFO("GetValue: key=%ld",key.ToString());
  bool temp = false;
  bool *is_repeat = &temp;
  LeafPage *leaf = GetLeaf(key, is_repeat);
  if (*is_repeat) {
    for (int i = 0; i < leaf->GetSize(); i++) {
      if (comparator_(leaf->KeyAt(i), key) == 0) {
        result->push_back(leaf->ValueAt(i));
        return true;
      }
    }
    return false;
  }
  // LOG_INFO("is_repeat is false");
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *transaction) -> bool {
  // LOG_INFO("@Insert key is: %ld",key.ToString());
  // not exist root
  if (root_page_id_ == INVALID_PAGE_ID) {
    auto root_page = buffer_pool_manager_->NewPage(&root_page_id_);

    auto root = reinterpret_cast<LeafPage *>(root_page->GetData());
    root->Init(root_page_id_, INVALID_PAGE_ID, leaf_max_size_);
    InsertNode(reinterpret_cast<BPlusTreePage *>(root), key, value);
    UpdateRootPageId(0);

    if (!buffer_pool_manager_->UnpinPage(root_page_id_, true)) {
      LOG_DEBUG("Insert: unpin root page failed");
    }
    return true;
  }

  // Get leaf1
  bool temp = false;
  bool *is_repeat = &temp;
  LeafPage *leaf1 = GetLeaf(key, is_repeat);
  if (*is_repeat) {
    // Duplicate key, return false
    if (!buffer_pool_manager_->UnpinPage(leaf1->GetPageId(), false)) {
      LOG_DEBUG("Insert: unpin leaf1 page failed");
    }
    // if (!buffer_pool_manager_->UnpinPage(root_page_id_, false)) {
    //   LOG_DEBUG("Insert: unpin root page failed");
    // }
    return false;
  }

  auto page1 = reinterpret_cast<BPlusTreePage *>(leaf1);

  // page1 is not full, insert directly
  if (page1->GetSize()+1 < leaf_max_size_) {
    InsertNode(page1, key, value);
    // if (!buffer_pool_manager_->UnpinPage(root_page_id_, false)) {
    //   LOG_DEBUG("Insert: unpin root page failed");
    // }
    if (!buffer_pool_manager_->UnpinPage(page1->GetPageId(), true)) {
      LOG_DEBUG("Insert: unpin leaf1 page failed");
    }
    return true;
  }
  // page1 is full Create a new leaf
  page_id_t leaf2_page_id;
  auto leaf2_page = buffer_pool_manager_->NewPage(&leaf2_page_id);
  auto leaf2 = reinterpret_cast<LeafPage *>(leaf2_page->GetData());
  leaf2->Init(leaf2_page_id, leaf1->GetParentPageId(), leaf_max_size_);
  auto page2 = reinterpret_cast<BPlusTreePage *>(leaf2);
  // splite
  InsertNode(page1, key, value);
  leaf2->SetNextPageId(leaf1->GetNextPageId());
  leaf1->SetNextPageId(leaf2->GetPageId());
  // move to leaf2
  for(int i=leaf1->GetSize()/2;i<leaf1->GetSize();++i){
    leaf2->SetPairAt(leaf2->GetSize(), {leaf1->KeyAt(i), leaf1->ValueAt(i)});
    leaf2->IncreaseSize(1);
  }
  int increase_size = leaf1->GetSize() - leaf1->GetSize()/2 ;
  leaf1->IncreaseSize(-increase_size);
  // end splite

  KeyType first_key = leaf2->KeyAt(0);
  RID rid;
  rid.Set(leaf2->GetPageId(), leaf2->GetPageId() & 0xFFFFFFFF);
  InsertParent(page1, page2, first_key, rid);

  // if (!buffer_pool_manager_->UnpinPage(root_page_id_, false)) {
  //   LOG_DEBUG("Insert: unpin root page failed");
  // }
  if (!buffer_pool_manager_->UnpinPage(page1->GetPageId(), true)) {
    LOG_DEBUG("Insert: unpin leaf1 page failed");
  }
  if (!buffer_pool_manager_->UnpinPage(page2->GetPageId(), true)) {
    LOG_DEBUG("Insert: unpin leaf2 page failed");
  }
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertNode(BPlusTreePage *node, const KeyType &key, const ValueType &value) {
  // LOG_INFO("@InsertNode p%d,%ld",node->GetPageId(),key.ToString());
  int value_int = value.GetSlotNum();
  if (node->IsLeafPage()) {
    auto leaf = reinterpret_cast<LeafPage *>(node);

    // leaf is empty or key < first key in leaf
    if (leaf->GetSize() == 0 || comparator_(key, leaf->KeyAt(0)) == -1) {
      // LOG_INFO("Insert Leaf key < first key in leaf %ld", key.ToString());
      if (!leaf->SetPairAt(0, MappingType(key, value))) {
        LOG_DEBUG("InsertNode: set pair failed 2");
      }
      leaf->IncreaseSize(1);
      return;
    }

    for (int i = 1; i < leaf->GetSize(); i++) {
      // it->first > key
      if (comparator_(key, leaf->KeyAt(i)) == -1) {
        // LOG_INFO("Insert Leaf key < it->first %ld", key.ToString());
        if (!leaf->SetPairAt(i, MappingType(key, value))) {
          LOG_DEBUG("InsertNode: set pair failed 3");
        }
        leaf->IncreaseSize(1);
        return;
      }
    }
    // key > last key in leaf
    if (!leaf->SetPairAt(leaf->GetSize(), MappingType(key, value))) {
      // LOG_INFO("Insert key > last key in leaf %ld ,leaf size is: %d", key.ToString(), leaf->GetSize());
      LOG_DEBUG("InsertNode: set pair failed 6");
    }
    leaf->IncreaseSize(1);
    return;
  }

  auto internal = reinterpret_cast<InternalPage *>(node);
  //internal is no key or key < first key in internal
  if (internal->GetSize()==1 || comparator_(key, internal->KeyAt(1)) == -1) {
    if (!internal->SetPairAt(1, std::make_pair(key, value_int))) {
      LOG_DEBUG("InsertNode: set pair failed 4");
    }
    internal->IncreaseSize(1);
    return;
  }
  // insert
  for (int i = 2; i < internal->GetSize(); i++) {
    // key < it->first
    if (comparator_(key, internal->KeyAt(i)) == -1) {
      if (!internal->SetPairAt(i, std::make_pair(key, value_int))) {
        LOG_DEBUG("InsertNode: set pair failed 5");
      }
      internal->IncreaseSize(1);
      return;
    }
  }
  // key > last key in internal
  // fist key is invalid so size+1
  if (!internal->SetPairAt(internal->GetSize(), std::make_pair(key, value_int))) {
    LOG_DEBUG("InsertNode: set pair failed 7");
  }
  internal->IncreaseSize(1);
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetLeaf(const KeyType &key, bool *is_repeat) -> LeafPage * {
  // LOG_INFO("@GetLeaf key:%ld",key.ToString());
  auto root_page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto node = reinterpret_cast<BPlusTreePage *>(root_page->GetData());

  while (!node->IsLeafPage()) {
    bool found = false;
    auto node_as_internal = reinterpret_cast<InternalPage *>(node);
    page_id_t next_page_id;

    // find key <= it->first
    for (int i = 1; i < node_as_internal->GetSize(); i++) {
      if (comparator_(key, node_as_internal->KeyAt(i)) != 1) {
        if (comparator_(key, node_as_internal->KeyAt(i)) == 0) {
          next_page_id = node_as_internal->ValueAt(i);
        } else {
          next_page_id = node_as_internal->ValueAt(i-1);
        }
        found = true;
        break;
      }
    }
    // not exist key <= it->first
    if (!found) {
      next_page_id = node_as_internal->ValueAt(node_as_internal->GetSize() - 1);
    }
    
    if (!buffer_pool_manager_->UnpinPage(node_as_internal->GetPageId(), false)) {
      LOG_DEBUG("GetLeaf: unpin internal page failed");
    }
    auto node_page = buffer_pool_manager_->FetchPage(next_page_id);
    if(node_page==nullptr){
      LOG_DEBUG("FetchPage %d not success",next_page_id);
    }
    node = reinterpret_cast<BPlusTreePage *>(node_page->GetData());
  }

  auto leaf = reinterpret_cast<LeafPage *>(node);
  // check if key exists in leaf
  for (int i = 0; i < leaf->GetSize(); i++) {
    if (comparator_(key, leaf->KeyAt(i)) == 0) {
      // LOG_INFO("key %ld exist in leaf %d",key.ToString(),leaf->GetPageId());
      *is_repeat = true;
      return leaf;
    }
  }
  // LOG_INFO("key %ld not exist in leaf %d",key.ToString(),leaf->GetPageId());
  *is_repeat = false;
  return leaf;
}

// INDEX_TEMPLATE_ARGUMENTS
// void BPLUSTREE_TYPE::SplitTree(BPlusTreePage *page1, BPlusTreePage *page2){
//   LOG_INFO("@SplitTree p%d,p%d",page1->GetPageId(),page2->GetPageId());
//   int half_index = 0;
//   if (page1->IsLeafPage()) {
//     auto leaf1 = reinterpret_cast<LeafPage *>(page1);
//     auto leaf2 = reinterpret_cast<LeafPage *>(page2);
//     leaf2->SetNextPageId(leaf1->GetNextPageId());
//     leaf1->SetNextPageId(leaf2->GetPageId());
//     // Rounded up
//     // include inert key so leaf1->GetSize() + 1
//     half_index = (leaf1->GetSize() + 1) / 2;
//     // move to leaf2
//     for(int i=half_index;i<leaf1->GetSize();++i){
//       leaf2->SetPairAt(leaf2->GetSize(), {leaf1->KeyAt(i), leaf1->ValueAt(i)});
//       leaf2->IncreaseSize(1);
//     }
//     int increase_size = leaf1->GetSize() - half_index ;
//     leaf1->IncreaseSize(-increase_size);
//     return;
//   }
//   // page1,2 is internal page
//   auto internal1 = reinterpret_cast<InternalPage *>(page1);
//   auto internal2 = reinterpret_cast<InternalPage *>(page2);
//   // Rounded up and insert key also need conisdier so +2
//   half_index = (internal_max_size_ + 2) / 2;
//   auto size_page1 = internal1->GetSize();
//   for(int i=half_index;i<size_page1;++i){
//     // change parent page id
//     auto move_page = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(internal1->ValueAt(i))->GetData());
//     move_page->SetParentPageId(internal2->GetPageId());
//     if (!buffer_pool_manager_->UnpinPage(move_page->GetPageId(), true)) {
//       LOG_DEBUG("Unpin page failed");
//     }
//     //move to leaf2
//     // LOG_INFO("page %d move %ld limited is: %d",internal1->GetPageId(),internal1->KeyAt(i).ToString(),internal1->GetSize());
//     // value is 1 bigger than key so keyat i-1
//     internal2->SetPairAt(internal2->GetSize(), {internal1->KeyAt(i), internal1->ValueAt(i)});
//     internal2->IncreaseSize(1);
//   }
//   int increase_size =size_page1 - half_index-1;
//   // LOG_INFO("size is %d,increase size is: %d,half is: %d",internal1->GetSize(),increase_size,half_index);
//   internal1->IncreaseSize(-increase_size);
//   // future
//   // LOG_INFO("page p%d size is:%d",internal1->GetPageId(),internal1->GetSize());
//   for(int i=0;i<internal1->GetSize();++i){
//     // LOG_INFO("  key %ld",internal1->KeyAt(i).ToString());
//   }
//   return;
// }

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertParent(BPlusTreePage *page1, BPlusTreePage *page2, const KeyType &key, const ValueType &value) {
  // LOG_INFO("@InsertParent p%d,p%d,key: %ld",page1->GetPageId(),page2->GetPageId(),key.ToString());
  if (page1->IsRootPage()) {
    // Create a new root
    page_id_t root_page_id;
    auto new_root_page = buffer_pool_manager_->NewPage(&root_page_id);
    auto new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(root_page_id, INVALID_PAGE_ID, internal_max_size_);

    // Insert page1,key,page2 into new root
    KeyType invalid_key;
    invalid_key.SetFromInteger(-1);
    if (!new_root->SetPairAt(0, std::make_pair(invalid_key, page1->GetPageId()))) {
      LOG_DEBUG("InsertParent: set pair failed 1");
    }
    new_root->IncreaseSize(1);
    if (!new_root->SetPairAt(1, std::make_pair(key, page2->GetPageId()))) {
      LOG_DEBUG("InsertParent: set pair failed 2");
    }
    new_root->IncreaseSize(1);

    page1->SetParentPageId(root_page_id);
    page2->SetParentPageId(root_page_id);
    root_page_id_ = root_page_id;
    UpdateRootPageId(1);
    // future
    // LOG_INFO("page p%d size is:%d",new_root->GetPageId(),new_root->GetSize());
    for(int i=0;i<new_root->GetSize();++i){
      // LOG_INFO("  key %ld",new_root->KeyAt(i).ToString());
    }
    if (!buffer_pool_manager_->UnpinPage(new_root->GetPageId(), true)) {
      LOG_DEBUG("InsertParent: unpin root page failed");
    }
    return;
  }
  
  auto parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(page1->GetParentPageId())->GetData());
  if (parent->GetSize() == internal_max_size_) {
    // create a new page
    page_id_t parent_page_prime_id;
    auto parent_page_prime = buffer_pool_manager_->NewPage(&parent_page_prime_id);
    auto parent_prime = reinterpret_cast<InternalPage *>(parent_page_prime->GetData());
    parent_prime->Init(parent_page_prime_id, parent->GetParentPageId(), internal_max_size_);

    // split
    InsertNode(reinterpret_cast<BPlusTreePage*>(parent), key, value);
    auto half_index = parent->GetSize() / 2;
    auto k_prime = parent->KeyAt(half_index);
    for(int i=half_index;i<parent->GetSize();++i){
      // change parent page id
      auto move_page = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(parent->ValueAt(i))->GetData());
      move_page->SetParentPageId(parent_prime->GetPageId());
      if (!buffer_pool_manager_->UnpinPage(move_page->GetPageId(), true)) {
        LOG_DEBUG("Unpin page failed");
      }
      //move to parent_prime
      parent_prime->SetPairAt(parent_prime->GetSize(), {parent->KeyAt(i), parent->ValueAt(i)});
      parent_prime->IncreaseSize(1);
    }
    int increase_size =parent->GetSize() - half_index;
    // LOG_INFO("size is %d,increase size is: %d,half is: %d",internal1->GetSize(),increase_size,half_index);
    parent->IncreaseSize(-increase_size);
    // future
    // LOG_INFO("page p%d size is:%d",internal1->GetPageId(),internal1->GetSize());
    // for(int i=0;i<internal1->GetSize();++i){
    //   // LOG_INFO("  key %ld",internal1->KeyAt(i).ToString());
    // }
    // end split

    // page2->SetParentPageId(parent_page_prime_id);
    // parent->KeyAt(parent->GetSize() - 1);
    RID rid;
    rid.Set(parent_page_prime_id, parent_page_prime_id & 0xFFFFFFFF);
    // parent->IncreaseSize(-1);
    InsertParent(parent, parent_prime, k_prime, rid);
    if (!buffer_pool_manager_->UnpinPage(parent_page_prime_id, true)) {
      LOG_DEBUG("InsertParent: Unpin page failed");
    }
    if (!buffer_pool_manager_->UnpinPage(page1->GetParentPageId(), true)) {
      LOG_DEBUG("InsertParent: Unpin page failed");
    }
    return;
  }
  // parent is not full
  InsertNode(parent, key, value);
  if (!buffer_pool_manager_->UnpinPage(page1->GetParentPageId(), true)) {
    LOG_DEBUG("InsertParent: Unpin page failed");
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immdiately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  LOG_INFO("Remove: key=%ld",key.ToString());
  if (root_page_id_ == INVALID_PAGE_ID) {
    return;
  }
  bool temp = false;
  bool *is_repeat = &temp;
  LeafPage *leaf = GetLeaf(key, is_repeat);
  if (!*is_repeat) {
    LOG_DEBUG("key not found");
  }
  RemoveEntry(reinterpret_cast<BPlusTreePage *>(leaf), key);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveEntry(BPlusTreePage *node1, const KeyType &key) {
  // leaf
  if (node1->IsLeafPage()) {
    auto leaf = reinterpret_cast<LeafPage *>(node1);
    if (!leaf->DeletePair(key, comparator_)) {
      LOG_DEBUG("RemoveEntry: delete pair failed");
    }

    if (leaf->IsRootPage()) {
      return;
    }
    if (leaf->GetSize() >= leaf->GetMinSize()) {
      return;
    }

    LeafPage *leaf_plus = nullptr;
    InternalPage *parent;

    parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(leaf->GetParentPageId())->GetData());
    // find leaf_plus
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      leaf_plus = leaf;
      leaf = reinterpret_cast<LeafPage *>(buffer_pool_manager_->FetchPage(leaf_plus->GetNextPageId())->GetData());
    } else {
      for (int i = 1; i < parent->GetSize(); i++) {
        if (comparator_(parent->KeyAt(i), key) == 0) {
          auto leaf_plus_page_id = parent->ValueAt(i);
          leaf_plus = reinterpret_cast<LeafPage *>(buffer_pool_manager_->FetchPage(leaf_plus_page_id)->GetData());
          break;
        }
      }
    }

    KeyType key_plus = leaf->KeyAt(0);
    // coalesce
    if (leaf->GetSize() + leaf_plus->GetSize() <= leaf->GetMaxSize()) {
      for (int i = 0; i < leaf->GetSize(); i++) {
        if (!leaf_plus->SetPairAt(leaf_plus->GetSize(), {leaf->KeyAt(i), leaf->ValueAt(i)})) {
          LOG_DEBUG("RemoveEntry: set pair failed");
        }
        leaf_plus->IncreaseSize(1);
      }
      leaf_plus->SetNextPageId(leaf->GetNextPageId());
      RemoveEntry(reinterpret_cast<BPlusTreePage *>(parent), key_plus);
      buffer_pool_manager_->DeletePage(leaf->GetPageId());
      if (!buffer_pool_manager_->UnpinPage(parent->GetPageId(), false)) {
        LOG_DEBUG("RemoveEntry: Unpin page failed");
      }
      if (!buffer_pool_manager_->UnpinPage(leaf_plus->GetPageId(), true)) {
        LOG_DEBUG("RemoveEntry: Unpin page failed");
      }
      return;
    }

    // redistribute
    KeyType last_key = leaf_plus->KeyAt(leaf_plus->GetSize() - 1);
    ValueType last_value = leaf_plus->ValueAt(leaf_plus->GetSize() - 1);
    leaf_plus->IncreaseSize(-1);
    if (!leaf->SetPairAt(0, {last_key, last_value})) {
      LOG_DEBUG("redistribute: set pair failed");
    }
    for (int i = 1; i < parent->GetSize(); i++) {
      if (comparator_(parent->KeyAt(i), last_key) == 0) {
        parent->SetKeyAt(i, last_key);
        break;
      }
    }
    if (!buffer_pool_manager_->UnpinPage(parent->GetPageId(), true)) {
      LOG_DEBUG("redistribute: Unpin page failed");
    }
    if (!buffer_pool_manager_->UnpinPage(leaf_plus->GetPageId(), true)) {
      LOG_DEBUG("redistribute: Unpin page failed");
    }
    if (!buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true)) {
      LOG_DEBUG("redistribute: Unpin page failed");
    }
    return;
  }

  // internal
  auto internal = reinterpret_cast<InternalPage *>(node1);
  if (!internal->DeletePair(key, comparator_)) {
    LOG_DEBUG("internal: delete pair failed");
  }

  if (internal->IsRootPage()) {
    // internal only has one child
    if (internal->GetSize() == 1) {
      auto child_page_id = internal->ValueAt(0);
      auto child = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(child_page_id)->GetData());
      child->SetParentPageId(INVALID_PAGE_ID);
      root_page_id_ = child_page_id;
      UpdateRootPageId(1);
      if (!buffer_pool_manager_->UnpinPage(child_page_id, true)) {
        LOG_DEBUG("internal: unpin page failed");
      }
      buffer_pool_manager_->DeletePage(internal->GetPageId());
    }
    return;
  }

  KeyType key_plus;
  InternalPage *internal_plus = nullptr;
  auto *parent =
      reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(internal->GetParentPageId())->GetData());

  // find internal_plus
  for (int i = 0; i < parent->GetSize(); i++) {
    if (parent->ValueAt(i) == internal->GetPageId()) {
      if (i == 0) {
        key_plus = parent->KeyAt(i+1);
        auto internal_id = parent->ValueAt(i + 1);
        internal_plus = internal;
        internal = reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(internal_id)->GetData());
      } else {
        key_plus = parent->KeyAt(i);
        auto internal_plus_page_id = parent->ValueAt(i - 1);
        internal_plus =
            reinterpret_cast<InternalPage *>(buffer_pool_manager_->FetchPage(internal_plus_page_id)->GetData());
      }
      break;
    }
  }

  // coalesce
  if (internal->GetSize() + internal_plus->GetSize() <= internal->GetMaxSize()) {
    if (!internal_plus->SetPairAt(internal_plus->GetSize(), {key_plus, internal->ValueAt(0)})) {
      LOG_DEBUG("coalesce: set pair failed");
    }
    internal_plus->IncreaseSize(1);
    for (int i = 1; i < internal->GetSize(); i++) {
      if (!internal_plus->SetPairAt(internal_plus->GetSize(), {internal->KeyAt(i), internal->ValueAt(i)})) {
        LOG_DEBUG("coalesce: set pair failed1");
      }
      internal_plus->IncreaseSize(1);
    }
    RemoveEntry(reinterpret_cast<BPlusTreePage *>(parent), key_plus);
    buffer_pool_manager_->DeletePage(internal->GetPageId());
    if (!buffer_pool_manager_->UnpinPage(parent->GetPageId(), false)) {
      LOG_DEBUG("coalesce: unpin page failed");
    }
    if (!buffer_pool_manager_->UnpinPage(internal_plus->GetPageId(), true)) {
      LOG_DEBUG("coalesce: unpin page failed");
    }
    return;
  }

  // redistribute
  KeyType last_key = internal_plus->KeyAt(internal_plus->GetSize() - 1);
  auto last_value = internal_plus->ValueAt(internal_plus->GetSize() - 1);
  // delete last key and value in internal_plus
  internal_plus->IncreaseSize(-1);
  if (!internal->SetPairAt(0, {key_plus, last_value})) {
    LOG_DEBUG("redistribute: set pair failed");
  }
  for (int i = 1; i < parent->GetSize(); i++) {
    if (comparator_(parent->KeyAt(i), key) == 0) {
      parent->SetKeyAt(i, last_key);
      break;
    }
  }
  if (!buffer_pool_manager_->UnpinPage(parent->GetPageId(), true)) {
    LOG_DEBUG("redistribute: unpin page failed");
  }
  if (!buffer_pool_manager_->UnpinPage(internal_plus->GetPageId(), true)) {
    LOG_DEBUG("redistribute: unpin page failed");
  }
  if (!buffer_pool_manager_->UnpinPage(internal->GetPageId(), true)) {
    LOG_DEBUG("redistribute: unpin page failed");
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leaftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE { return INDEXITERATOR_TYPE(); }

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE { return INDEXITERATOR_TYPE(); }

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE { return INDEXITERATOR_TYPE(); }

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t { return root_page_id_; }

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      defualt value is false. When set to true,
 * insert a record <index_name, root_page_id> into header page instead of
 * updating it.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateRootPageId(int insert_record) {
  auto *header_page = static_cast<HeaderPage *>(buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record != 0) {
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  } else {
    // update root_page_id in header_page
    header_page->UpdateRecord(index_name_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
}

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name, Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, transaction);
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager *buffer_pool_manager_, const std::string &outf) {
  if (IsEmpty()) {
    LOG_WARN("Draw an empty tree");
    return;
  }
  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  ToGraph(reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData()),
          buffer_pool_manager_, out);
  out << "}" << std::endl;
  out.flush();
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager *buffer_pool_manager_) {
  if (IsEmpty()) {
    LOG_WARN("Print an empty tree");
    return;
  }
  ToString(reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData()),
           buffer_pool_manager_);
}

/**
 * This method is used for debug only, You don't need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param buffer_pool_manager_
 * @param out
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(BPlusTreePage *page, BufferPoolManager *buffer_pool_manager_, std::ofstream &out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        out << inner->KeyAt(i);
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page =
          reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, buffer_pool_manager_, out);
      if (i > 0) {
        auto sibling_page =
            reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 * @tparam KeyType
 * @tparam ValueType
 * @tparam KeyComparator
 * @param page
 * @param buffer_pool_manager_
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToString(BPlusTreePage *page, BufferPoolManager *buffer_pool_manager_) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(internal->ValueAt(i))->GetData()),
               buffer_pool_manager_);
    }
  }
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
