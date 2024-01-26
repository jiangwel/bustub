#include <string>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/header_page.h"

// #define PrintEntryFunInfo
// #define PrintGetLeafInfo
// #define PrintLogInfo
// #define PrintThreadInfo

// #define TestCode

namespace bustub {
INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, BufferPoolManager *buffer_pool_manager, const KeyComparator &comparator,
                          int leaf_max_size, int internal_max_size)
    : index_name_(std::move(name)),
      buffer_pool_manager_(buffer_pool_manager),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      root_page_id_(INVALID_PAGE_ID) {
        #ifdef PrintEntryFunInfo
        LOG_INFO("BPlusTree: leaf_max_size_=%d, internal_max_size_=%d", leaf_max_size_, internal_max_size_);
        #endif
      }

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool { return root_page_id_ == INVALID_PAGE_ID; }
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
  #ifdef PrintLogInfo
  LOG_INFO("GetValue: key=%ld", key.ToString());
  #endif
  bool found = false;
  auto[leaf,is_root_latched] = GetLeaf(key, OperateType::Find, transaction);
  for (int i = 0; i < leaf->GetSize(); i++) {
    if (comparator_(key, leaf->KeyAt(i)) == 0) {
      result->push_back(leaf->ValueAt(i));
      found = true;
      break;
    }
  }
  auto leaf_page = buffer_pool_manager_->FetchPage(leaf->GetPageId());
  leaf_page->RUnlatch();
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return found;
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReleaseResourcesd(Transaction *transaction){
  for(auto p : *transaction->GetPageSet()){
    p->WUnlatch();
    buffer_pool_manager_->UnpinPage(p->GetPageId(), true);
  }
  transaction->GetPageSet()->clear();
  for (auto p : *transaction->GetDeletedPageSet()) {
    buffer_pool_manager_->DeletePage(p);
  }
  transaction->GetDeletedPageSet()->clear();
}
/*****************************************************************************
 * INSERTION
 *****************************************************************************/

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::MakeRoot(const KeyType &key, const ValueType &value){
  page_id_t new_root_page_id;
  auto new_root = reinterpret_cast<LeafPage *>(buffer_pool_manager_->NewPage(&new_root_page_id)->GetData());
  root_page_id_ = new_root_page_id;
  new_root->Init(root_page_id_, INVALID_PAGE_ID, leaf_max_size_);
  InsertNode(reinterpret_cast<BPlusTreePage *>(new_root), key, value);
  UpdateRootPageId(0);
  buffer_pool_manager_->UnpinPage(new_root->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertInFillNode(LeafPage *leaf1,const KeyType &key, const ValueType &value,Transaction *transaction){
  page_id_t new_node_id;
  auto new_page = buffer_pool_manager_->NewPage(&new_node_id);
  new_page->WLatch();
  auto page_set = transaction->GetPageSet();
  page_set->push_back(new_page);

  auto leaf2 = reinterpret_cast<LeafPage *>(new_page->GetData());
  leaf2->Init(new_node_id, leaf1->GetParentPageId(), leaf_max_size_);

  // splite
  leaf2->SetNextPageId(leaf1->GetNextPageId());
  leaf1->SetNextPageId(leaf2->GetPageId());
  // move to leaf2
  for (int i = leaf1->GetSize() / 2; i < leaf1->GetSize(); ++i) {
    leaf2->SetPairAt(leaf2->GetSize(), {leaf1->KeyAt(i), leaf1->ValueAt(i)});
  }
  int increase_size = leaf1->GetSize() - leaf1->GetSize() / 2;
  leaf1->IncreaseSize(-increase_size);

  // update parent
  KeyType first_key = leaf2->KeyAt(0);
  RID rid(leaf2->GetPageId(), leaf2->GetPageId() & 0xFFFFFFFF);
  auto node2 = reinterpret_cast<BPlusTreePage *>(leaf2);
  auto node1 = reinterpret_cast<BPlusTreePage *>(leaf1);
  BPlusTree::InsertParent(node1, node2, first_key, rid, transaction);
}


/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value, Transaction *transaction) -> bool {
root_page_id_latch_.WLock();
#ifdef PrintEntryFunInfo
  auto key_value = key.ToString();
  LOG_INFO("@Insert key is: %ld rid.GetPageId:%d thread %zu", key_value, value.GetPageId(),
    std::hash<std::thread::id>{}(transaction->GetThreadId()));
#endif

  if (IsEmpty()) {
    MakeRoot(key,value);
    root_page_id_latch_.WUnlock();
    return true;
  }

  auto [leaf1, is_root_latched] = GetLeaf(key, OperateType::Insert, transaction);
  bool leaf1IsFull = leaf1->GetSize() + 1 == leaf_max_size_;
  
  bool is_duplicate_key= false;
  for (int i = 0; i < leaf1->GetSize(); i++) {
    if (comparator_(key, leaf1->KeyAt(i)) == 0) {
      is_duplicate_key = true;
      break;
    }
  }
  if (is_duplicate_key) {
    root_page_id_latch_.WUnlock();
    ReleaseResourcesd(transaction);
    return false;
  }

  auto node1 = reinterpret_cast<BPlusTreePage *>(leaf1);

  if (!leaf1IsFull) {
    root_page_id_latch_.WUnlock();
    InsertNode(node1, key, value);
    ReleaseResourcesd(transaction);
    return true;
  }

  InsertNode(node1, key, value);
  InsertInFillNode(leaf1,key,value,transaction);
  ReleaseResourcesd(transaction);
  return true;
}


INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertNode(BPlusTreePage *node, const KeyType &key, const ValueType &value) {
#ifdef PrintLogInfo
  LOG_INFO("InsertNode: key=%ld value=%d page id=%d thread %zu", key.ToString(), value.GetPageId(), node->GetPageId(), std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
  int value_int = value.GetSlotNum();
  // #insert leaf
  if (node->IsLeafPage()) {
    auto leaf = reinterpret_cast<LeafPage *>(node);

    // leaf is empty or key < first key in leaf
    if (leaf->GetSize() == 0 || comparator_(key, leaf->KeyAt(0)) == -1) {
      if (!leaf->SetPairAt(0, MappingType(key, value))) {
        LOG_DEBUG("InsertNode: set pair failed 2");
      }
      return;
    }

    for (int i = 1; i < leaf->GetSize(); i++) {
      // it->first > key
      if (comparator_(key, leaf->KeyAt(i)) == -1) {
        if (!leaf->SetPairAt(i, MappingType(key, value))) {
          LOG_DEBUG("InsertNode: set pair failed 3");
        }
        return;
      }
    }
    // key > last key in leaf
    if (!leaf->SetPairAt(leaf->GetSize(), MappingType(key, value))) {
      LOG_DEBUG("InsertNode: set pair failed 6");
    }
    return;
  }
  // #end insert leaf

  // #insert internal
  auto internal = reinterpret_cast<InternalPage *>(node);
  // internal is no key or key < first key in internal
  if (internal->GetSize() == 1 || comparator_(key, internal->KeyAt(1)) == -1) {
    if (!internal->SetPairAt(1, std::make_pair(key, value_int))) {
      LOG_DEBUG("InsertNode: set pair failed 4");
    }
    return;
  }
  // insert
  // fist key is invalid and index 1 compared,so start at 2
  for (int i = 2; i < internal->GetSize(); i++) {
    // key < it->first
    if (comparator_(key, internal->KeyAt(i)) == -1) {
      if (!internal->SetPairAt(i, std::make_pair(key, value_int))) {
        LOG_DEBUG("InsertNode: set pair failed 5");
      }
      return;
    }
  }
  // key > last key in internal
  // fist key is invalid so size+1
  if (!internal->SetPairAt(internal->GetSize(), std::make_pair(key, value_int))) {
    LOG_DEBUG("InsertNode: set pair failed 7");
  }
  // #end insert internal
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetLeaf(const KeyType& key, OperateType operator_type, Transaction* transaction)
-> std::tuple<LeafPage*, bool> {
  auto is_root_latched = true;
  auto curr_page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto curr_node = reinterpret_cast<BPlusTreePage*>(curr_page->GetData());


  if (operator_type== OperateType::Find) {
    curr_page->RLatch();
  }
  else {
    curr_page->WLatch();
    transaction->GetPageSet()->push_back(curr_page);
  }

  while (!curr_node->IsLeafPage()) {
    bool found = false;
    auto node_as_internal = reinterpret_cast<InternalPage*>(curr_node);
    page_id_t next_page_id;

    // find key <= input_kay
    for (int i = 1; i < node_as_internal->GetSize(); i++) {
      if (comparator_(key, node_as_internal->KeyAt(i)) != 1) {
        // is key == input_kay?
        next_page_id = comparator_(key, node_as_internal->KeyAt(i)) == 0 ? node_as_internal->ValueAt(i)
          : node_as_internal->ValueAt(i - 1);
        found = true;
        break;
      }
    }
    next_page_id = !found ? (node_as_internal->ValueAt(node_as_internal->GetSize() - 1)) : next_page_id;
    auto child_page = buffer_pool_manager_->BufferPoolManager::FetchPage(next_page_id);
    auto child_node = reinterpret_cast<BPlusTreePage*>(child_page->GetData());

    if (operator_type == OperateType::Find) {
      child_page->RLatch();
      curr_page->RUnlatch();
      buffer_pool_manager_->UnpinPage(curr_page->GetPageId(), false);
    }
    else {
      child_page->WLatch();
      if (IsSafe(child_node, operator_type)) {
        ReleaseResourcesd(transaction);
      }
      transaction->GetPageSet()->push_back(child_page);
    }

    curr_page = child_page;
    curr_node = child_node;
  }
  return { reinterpret_cast<LeafPage*>(curr_node), is_root_latched };
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsSafe(BPlusTreePage* node, OperateType op) -> bool {
  int add_num = node->IsLeafPage() ? -1 : 0;
  if (op == OperateType::Insert) {
    return node->GetSize() < node->GetMaxSize()+add_num;
  }
  assert(op == OperateType::Delete);
  if (node->IsRootPage()) {
    return node->GetSize() >= 2;
  }
  return node->GetSize() > node->GetMinSize();
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertParent(BPlusTreePage *page1, BPlusTreePage *page2, const KeyType &key,
  const ValueType &value, Transaction *transaction) {
#ifdef PrintLogInfo
  LOG_INFO("InsertParent: key=%ld value=%d page1 id=%d page2 id=%d thread %zu", key.ToString(), value.GetPageId(),
    page1->GetPageId(), page2->GetPageId(), std::hash<std::thread::id>{}(transaction->GetThreadId()));
#endif
  auto page_set = transaction->GetPageSet();

  if (page1->IsRootPage()) {
    RenewRoot(page1,page2,key);
    return;
  }

  auto *parent = reinterpret_cast<InternalPage *>(buffer_pool_manager_->BufferPoolManager::FetchPage(page1->GetParentPageId())->GetData());
  bool is_parent_full = parent->GetSize() == parent->GetMaxSize();

  if (is_parent_full) {
    InsertInFillParent(page1,page2,parent,key,value,transaction);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return;
  }
root_page_id_latch_.WUnlock();
  InsertNode(parent, key, value);
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RenewRoot(BPlusTreePage *page1, BPlusTreePage *page2,const KeyType &key){
  page_id_t root_page_id;
  auto new_root = reinterpret_cast<InternalPage *>(buffer_pool_manager_->NewPage(&root_page_id)->GetData());
  new_root->Init(root_page_id, INVALID_PAGE_ID, internal_max_size_);

  // Insert page1,key,page2 into new root
  KeyType invalid_key;
  invalid_key.SetFromInteger(-1);
  if (!new_root->SetPairAt(0, std::make_pair(invalid_key, page1->GetPageId()))) {
    LOG_DEBUG("InsertParent: set pair failed 1");
  }
  if (!new_root->SetPairAt(1, std::make_pair(key, page2->GetPageId()))) {
    LOG_DEBUG("InsertParent: set pair failed 2");
  }
  page1->SetParentPageId(root_page_id);
  page2->SetParentPageId(root_page_id);
  root_page_id_ = root_page_id;
  UpdateRootPageId(1);
root_page_id_latch_.WUnlock();
  buffer_pool_manager_->UnpinPage(new_root->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertInFillParent(BPlusTreePage *page1, BPlusTreePage *page2,InternalPage * parent,const KeyType &key, const ValueType &value, Transaction *transaction){
  page_id_t parent_page_prime_id;
  auto parent_prime_page = buffer_pool_manager_->NewPage(&parent_page_prime_id);
  auto parent_prime = reinterpret_cast<InternalPage *>(parent_prime_page->GetData());
  parent_prime->Init(parent_page_prime_id, parent->GetParentPageId(), internal_max_size_);

  // split
  InsertNode(reinterpret_cast<BPlusTreePage *>(parent), key, value);
  auto half_index = parent->GetSize() / 2;
  auto k_prime = parent->KeyAt(half_index);
  for (int i = half_index; i < parent->GetSize(); ++i) {
    // change parent page id
    auto move_page =
      reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(parent->ValueAt(i))->GetData());
    //LOG_INFO("page--");
    move_page->SetParentPageId(parent_prime->GetPageId());
    if (!buffer_pool_manager_->UnpinPage(move_page->GetPageId(), true)) {
      LOG_DEBUG("Unpin page failed");
    }
    //LOG_INFO("page++");
    // move to parent_prime
    parent_prime->SetPairAt(parent_prime->GetSize(), {parent->KeyAt(i), parent->ValueAt(i)});
  }
  int increase_size = parent->GetSize() - half_index;
  parent->IncreaseSize(-increase_size);
  // end split

  RID rid(parent_page_prime_id, parent_page_prime_id & 0xFFFFFFFF);

  InsertParent(parent, parent_prime, k_prime, rid, transaction);
  buffer_pool_manager_->UnpinPage(parent_prime->GetPageId(), true);
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
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* transaction) {
  root_page_id_latch_.WLock();
#ifdef PrintEntryFunInfo
  auto key_value = key.ToString();
  LOG_INFO("@Remove: key=%ld thread: %zu", key_value, std::hash<std::thread::id>{}(transaction->GetThreadId()));
#endif
  if (this->IsEmpty()) {
    root_page_id_latch_.WUnlock();
    return;
  }
  auto [leaf, is_root_lacthed] = BPlusTree::GetLeaf(key, OperateType::Delete, transaction);
  BPlusTree::RemoveEntry(reinterpret_cast<BPlusTreePage*>(leaf), key, transaction);
  root_page_id_latch_.WUnlock();
  ReleaseResourcesd(transaction);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveEntry(BPlusTreePage* node, const KeyType& key, Transaction* transaction) {
  auto page_set = transaction->GetPageSet();
  LeafPage *leaf = nullptr;
  InternalPage *internal = nullptr;
  if(node->IsLeafPage()){
    leaf = reinterpret_cast<LeafPage *>(node);
    leaf->DeletePair(key, comparator_);
  }
  else {
    internal = reinterpret_cast<InternalPage*>(node);
    internal->DeletePair(key, comparator_);
  }
  
  if (node->IsRootPage()) {
    if (node->IsLeafPage() && node->GetSize() == 0) {
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId(0);
      transaction->AddIntoDeletedPageSet(node->GetPageId());
      return;
    }
    bool is_root_need_adjust = !node->IsLeafPage() && node->GetSize() == 1;
    if (is_root_need_adjust) {
      ReplaceRootByChildren(reinterpret_cast<InternalPage*>(node),transaction);
    }
    return;
  }
  
  bool is_key_enough = node->GetSize() >= node->GetMinSize();
  if (is_key_enough) {
    return;
  }

  auto* parent =
    reinterpret_cast<InternalPage*>(buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
  int sibling_idx = GetSiblingIdx(parent, node->GetPageId());
  bool is_i_plus_before_i = parent->ValueAt(sibling_idx) < node->GetPageId();
  auto sibling_page = buffer_pool_manager_->FetchPage(parent->ValueAt(sibling_idx));
  auto* node_plus = reinterpret_cast<BPlusTreePage*>(sibling_page->GetData());
  auto key_plus = parent->KeyAt(is_i_plus_before_i ? sibling_idx + 1 : sibling_idx);

  sibling_page->WLatch();
  page_set->push_back(sibling_page);

  bool is_able_to_coalesce = node_plus->GetSize() + node->GetSize() <= node->GetMaxSize();

  if (is_able_to_coalesce) {
    Coalesce(is_i_plus_before_i, node, node_plus, key_plus, transaction);
  }
  else {
    Redistribute(node, node_plus, parent, is_i_plus_before_i, key_plus);
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ReplaceRootByChildren(InternalPage* old_root,Transaction* transaction) {
  auto new_root_id = old_root->ValueAt(0);
  old_root->SetSize(0);

  auto new_root = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(new_root_id)->GetData());
  new_root->SetParentPageId(INVALID_PAGE_ID);

  root_page_id_ = new_root_id;
  UpdateRootPageId(0);

  transaction->AddIntoDeletedPageSet(old_root->GetPageId());
  buffer_pool_manager_->UnpinPage(new_root_id, true);
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetSiblingIdx(InternalPage* parent_page, const int page_id)->int {
  int sibling_page_idx = -1;
  for (int i = 0; i < parent_page->GetSize(); i++) {
    if (parent_page->ValueAt(i) == page_id) {
      sibling_page_idx = i == 0 ? i + 1 : i - 1;
      break;
    }
  }
  return sibling_page_idx;
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Coalesce(bool is_sibling_brother, BPlusTreePage* node, BPlusTreePage* sibling_page, const KeyType& key_plus, Transaction* transaction) {
  if (!is_sibling_brother) {
    std::swap(node, sibling_page);
  }
  if (node->IsLeafPage()) {
    auto leaf = reinterpret_cast<LeafPage*>(node);
    auto leaf_sibling_page = reinterpret_cast<LeafPage*>(sibling_page);
    for (int i = 0; i < leaf->GetSize(); i++) {
      // leaf_sibling_page->SetPairAt(i+leaf_sibling_page->GetSize(), { leaf->KeyAt(i), leaf->ValueAt(i) });
      leaf_sibling_page->SetPairAt(leaf_sibling_page->GetSize(), { leaf->KeyAt(i), leaf->ValueAt(i) });
    }
    leaf->SetSize(0);
    leaf_sibling_page->SetNextPageId(leaf->GetNextPageId());
  }
  else {
    auto internal = reinterpret_cast<InternalPage*>(node);
    auto internal_sibling_page = reinterpret_cast<InternalPage*>(sibling_page);

    internal_sibling_page->SetPairAt(internal_sibling_page->GetSize(), { key_plus, internal->ValueAt(0) });

    auto child =
      reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(internal->ValueAt(0))->GetData());
    child->SetParentPageId(internal_sibling_page->GetPageId());
    buffer_pool_manager_->UnpinPage(child->GetPageId(), true);

    for (int i = 1; i < internal->GetSize(); i++) {
    // for (int i = 0; i < internal->GetSize(); i++) {
      // internal_sibling_page->SetPairAt(i+internal_sibling_page->GetSize(), { internal->KeyAt(i), internal->ValueAt(i) });
      internal_sibling_page->SetPairAt(internal_sibling_page->GetSize(), { internal->KeyAt(i), internal->ValueAt(i) });
      auto child =
        reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(internal->ValueAt(i))->GetData());
      child->SetParentPageId(internal_sibling_page->GetPageId());
      buffer_pool_manager_->UnpinPage(child->GetPageId(), true);
    }
    internal->SetSize(0);
  }

  auto parent = reinterpret_cast<InternalPage*>(buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
  RemoveEntry(parent, key_plus, transaction);

  // Delete node
  transaction->AddIntoDeletedPageSet(node->GetPageId());
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Redistribute(BPlusTreePage* node, BPlusTreePage* sib_node, InternalPage* parent, bool is_i_plus_before_i, KeyType key_plus) {
  KeyType temp_key;
  ValueType temp_value;
  if (node->IsLeafPage()) {
    auto leaf = reinterpret_cast<LeafPage*>(node);
    auto leaf_sibling_page = reinterpret_cast<LeafPage*>(sib_node);

    if (is_i_plus_before_i) {
      temp_key = leaf_sibling_page->KeyAt(leaf_sibling_page->GetSize() - 1);
      temp_value = leaf_sibling_page->ValueAt(leaf_sibling_page->GetSize() - 1);
      leaf_sibling_page->DeletePair(temp_key, comparator_);
      leaf->SetPairAt(0, { temp_key, temp_value });
    }
    else {
      temp_key = leaf_sibling_page->KeyAt(0);
      temp_value = leaf_sibling_page->ValueAt(0);
      leaf_sibling_page->DeletePair(temp_key, comparator_);
      leaf->SetPairAt(leaf->GetSize(), { temp_key, temp_value });
      temp_key = leaf_sibling_page->KeyAt(0);
    }
  }
  else {
    auto internal = reinterpret_cast<InternalPage*>(node);
    auto internal_sibling_node = reinterpret_cast<InternalPage*>(sib_node);
    int temp_value;
    if (is_i_plus_before_i) {
      auto index = internal_sibling_node->GetSize() - 1;
      temp_key = internal_sibling_node->KeyAt(index);
      temp_value = internal_sibling_node->ValueAt(index);
      auto child_node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(temp_value)->GetData());
      child_node->SetParentPageId(internal->GetPageId());
      buffer_pool_manager_->UnpinPage(child_node->GetPageId(), true);

      internal_sibling_node->DeletePair(temp_key, comparator_);
      internal->SetPairAt(0, { temp_key, temp_value });
    }
    else {
      temp_key = internal_sibling_node->KeyAt(0);
      temp_value = internal_sibling_node->ValueAt(0);

      auto child_node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(temp_value)->GetData());
      child_node->SetParentPageId(internal->GetPageId());
      buffer_pool_manager_->UnpinPage(child_node->GetPageId(), true);

      internal_sibling_node->DeletePair(temp_key, comparator_);
      internal->SetPairAt(internal->GetSize(), { temp_key, temp_value });
      temp_key = internal_sibling_node->KeyAt(0);
    }
  }
  parent->ReplaceKey(key_plus, temp_key, comparator_);
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
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE {
  auto node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData());
  while (!node->IsLeafPage()) {
    auto internal = reinterpret_cast<InternalPage*>(node);
    auto first_child_page_id = internal->ValueAt(0);
    node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(first_child_page_id)->GetData());
    if (!buffer_pool_manager_->UnpinPage(internal->GetPageId(), false)) {
      LOG_DEBUG("Begin: Unpin page failed");
    }
  }
  auto leaf = reinterpret_cast<LeafPage*>(node);
  return INDEXITERATOR_TYPE(leaf, 0, buffer_pool_manager_);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key) -> INDEXITERATOR_TYPE {
  auto [leaf, is_root_latched] = BPlusTree::GetLeaf(key, OperateType::Find, nullptr);
  int index = -1;
  for (int i = 0; i < leaf->GetSize(); i++){
    if (comparator_(key, leaf->KeyAt(i)) == 0) {
      index = i;
      break;
    }
  }
  return INDEXITERATOR_TYPE(leaf, index, buffer_pool_manager_);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE {
  auto node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData());
  while (!node->IsLeafPage()) {
    auto internal = reinterpret_cast<InternalPage*>(node);
    int last_index = internal->GetSize() - 1 < 0 ? 0 : internal->GetSize() - 1;
    auto last_child_page_id = internal->ValueAt(last_index);
    node = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(last_child_page_id)->GetData());
    if (!buffer_pool_manager_->UnpinPage(internal->GetPageId(), false)) {
      LOG_DEBUG("Begin: Unpin page failed");
    }
  }
  auto leaf = reinterpret_cast<LeafPage*>(node);
  return INDEXITERATOR_TYPE(leaf, leaf->GetSize(), buffer_pool_manager_);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  return root_page_id_;
}

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
  auto* header_page = static_cast<HeaderPage*>(buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record != 0) {
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  }
  else {
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
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name, Transaction* transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    BPlusTree::Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name, Transaction* transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    BPlusTree::Remove(index_key, transaction);
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* buffer_pool_manager_, const std::string& outf) {
  if (IsEmpty()) {
    LOG_WARN("Draw an empty tree");
    return;
  }
  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  ToGraph(reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData()),
    buffer_pool_manager_, out);
  out << "}" << std::endl;
  out.flush();
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* buffer_pool_manager_) {
  if (IsEmpty()) {
    LOG_WARN("Print an empty tree");
    return;
  }
  ToString(reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData()),
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
void BPLUSTREE_TYPE::ToGraph(BPlusTreePage* page, BufferPoolManager* buffer_pool_manager_, std::ofstream& out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto* leaf = reinterpret_cast<LeafPage*>(page);
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
  }
  else {
    auto* inner = reinterpret_cast<InternalPage*>(page);
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
      }
      else {
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
        reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, buffer_pool_manager_, out);
      if (i > 0) {
        auto sibling_page =
          reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(inner->ValueAt(i - 1))->GetData());
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
void BPLUSTREE_TYPE::ToString(BPlusTreePage* page, BufferPoolManager* buffer_pool_manager_) const {
  if (page->IsLeafPage()) {
    auto* leaf = reinterpret_cast<LeafPage*>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
      << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else {
    auto* internal = reinterpret_cast<InternalPage*>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(internal->ValueAt(i))->GetData()),
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