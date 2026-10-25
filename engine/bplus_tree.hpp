// bplus_tree.hpp - B+ Tree index: insert, search, range scan, delete.
//
// Warehouse meaning: the inventory index. Key = SKU id, value = bin location id.
// "Find the bin for SKU 48213" and "list every SKU between 1000 and 1999"
// should both be fast even with a million SKUs.
//
// How a B+ Tree differs from a plain binary tree:
//   * Each node holds MANY keys (up to maxKeys), so the tree is short and wide.
//     A million keys with maxKeys = 64 is only about 4 levels deep.
//   * Internal nodes hold only SEPARATOR keys, used for routing.
//   * ALL real (key, value) pairs live in the leaves.
//   * Leaves are linked left to right, so a range query finds the start leaf
//     once, then just walks along the chain.
//
// Rules this implementation keeps (checked by validate()):
//   * No node ever holds more than maxKeys keys.
//   * Every node except the root holds at least maxKeys / 2 keys.
//   * All leaves are at the same depth.
//   * In an internal node, child i holds keys k with keys[i-1] <= k < keys[i].
//
// Complexity (n keys, node capacity m):
//   find / insert / erase: O(log n)  (about log_m(n) levels, binary search inside each node)
//   range query returning r results: O(log n + r)
//   space: O(n)
//
// Delete: remove the key from its leaf. If that leaves a node with fewer than
// maxKeys / 2 keys, repair it on the way back up:
//   1. borrow a key from the left sibling if it has a spare one, else
//   2. borrow from the right sibling if it has a spare one, else
//   3. merge with a sibling (the parent loses one separator, which may in
//      turn make the parent underfull, and so on up to the root).
// If the root ends up with a single child, that child becomes the new root
// and the tree gets one level shorter.
//
// Written to compile with older compilers (C++11).

#pragma once
#include <climits>
#include <stdexcept>
#include <utility>
#include <vector>

class BPlusTree {
public:
    // maxKeys = most keys one node may hold (must be at least 3).
    explicit BPlusTree(int maxKeys = 4)
        : maxKeys_(checkOrder(maxKeys)), root_(new Node(true)), size_(0), comparisons_(0) {}

    ~BPlusTree() { destroy(root_); }

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    // Adds the key, or overwrites its value if it already exists.
    // Returns true if the key was new, false if it already existed.
    bool insert(int key, int value) {
        int upKey = 0;
        Node* newRight = nullptr;
        bool inserted = false;
        bool split = insertRec(root_, key, value, upKey, newRight, inserted);
        if (split) {
            // The old root split in two: grow the tree upward with a new root.
            Node* newRoot = new Node(false);
            newRoot->keys.push_back(upKey);
            newRoot->children.push_back(root_);
            newRoot->children.push_back(newRight);
            root_ = newRoot;
        }
        if (inserted) ++size_;
        return inserted;
    }

    // Removes the key. Returns true if it was there, false if not.
    bool erase(int key) {
        bool removed = false;
        eraseRec(root_, key, removed);
        if (!removed) return false;
        --size_;
        // If merging left the root with a single child, that child becomes the root.
        if (!root_->leaf && root_->keys.empty()) {
            Node* old = root_;
            root_ = old->children[0];
            delete old;
        }
        return true;
    }

    // Looks up a key. Returns true and fills `value` if found.
    bool find(int key, int& value) const {
        const Node* leaf = findLeaf(key);
        int i = lowerIdx(leaf->keys, key);
        if (i < static_cast<int>(leaf->keys.size())) {
            ++comparisons_;
            if (leaf->keys[i] == key) { value = leaf->values[i]; return true; }
        }
        return false;
    }

    bool contains(int key) const {
        int ignored = 0;
        return find(key, ignored);
    }

    // All (key, value) pairs with lo <= key <= hi, in sorted order.
    std::vector<std::pair<int, int> > range(int lo, int hi) const {
        std::vector<std::pair<int, int> > out;
        if (lo > hi) return out;
        const Node* leaf = findLeaf(lo);
        int i = lowerIdx(leaf->keys, lo);
        while (leaf != nullptr) {
            for (; i < static_cast<int>(leaf->keys.size()); ++i) {
                if (leaf->keys[i] > hi) return out;
                out.push_back(std::make_pair(leaf->keys[i], leaf->values[i]));
            }
            leaf = leaf->next;   // jump along the leaf chain
            i = 0;
        }
        return out;
    }

    // Everything, in sorted key order (walks the leaf chain).
    std::vector<std::pair<int, int> > toVector() const {
        std::vector<std::pair<int, int> > out;
        for (const Node* leaf = leftmostLeaf(); leaf != nullptr; leaf = leaf->next)
            for (std::size_t i = 0; i < leaf->keys.size(); ++i)
                out.push_back(std::make_pair(leaf->keys[i], leaf->values[i]));
        return out;
    }

    long long size() const { return size_; }
    bool empty() const { return size_ == 0; }
    int maxKeys() const { return maxKeys_; }

    // Number of levels, counting the leaf level. A lone leaf has height 1.
    int height() const {
        int h = 1;
        for (const Node* n = root_; !n->leaf; n = n->children[0]) ++h;
        return h;
    }

    // Key comparisons made by find/range/insert so far (for the benchmark).
    long long comparisons() const { return comparisons_; }
    void resetComparisons() { comparisons_ = 0; }

    // Checks every structural rule listed at the top. Used by the tests.
    bool validate() const {
        int leafDepth = -1;
        long long count = 0;
        if (!validateNode(root_, LLONG_MIN, LLONG_MAX, 1, leafDepth, count, true)) return false;
        if (count != size_) return false;

        // The leaf chain must visit every key exactly once, in strictly increasing order.
        long long chained = 0;
        bool havePrev = false;
        int prev = 0;
        for (const Node* leaf = leftmostLeaf(); leaf != nullptr; leaf = leaf->next) {
            for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
                if (havePrev && leaf->keys[i] <= prev) return false;
                prev = leaf->keys[i];
                havePrev = true;
                ++chained;
            }
        }
        return chained == size_;
    }

private:
    struct Node {
        bool leaf;
        std::vector<int> keys;
        std::vector<int> values;        // leaves only: values[i] belongs to keys[i]
        std::vector<Node*> children;    // internal only: children.size() == keys.size() + 1
        Node* next;                     // leaves only: the next leaf to the right
        explicit Node(bool isLeaf) : leaf(isLeaf), next(nullptr) {}
    };

    int maxKeys_;
    Node* root_;
    long long size_;
    mutable long long comparisons_;

    static int checkOrder(int m) {
        if (m < 3) throw std::invalid_argument("maxKeys must be at least 3");
        return m;
    }

    static void destroy(Node* n) {
        if (n == nullptr) return;
        if (!n->leaf)
            for (std::size_t i = 0; i < n->children.size(); ++i) destroy(n->children[i]);
        delete n;
    }

    // First index i with k[i] >= key (binary search, counts comparisons).
    int lowerIdx(const std::vector<int>& k, int key) const {
        int lo = 0, hi = static_cast<int>(k.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            ++comparisons_;
            if (k[mid] < key) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    // First index i with k[i] > key. In an internal node this is the child to follow.
    int upperIdx(const std::vector<int>& k, int key) const {
        int lo = 0, hi = static_cast<int>(k.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            ++comparisons_;
            if (k[mid] <= key) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    const Node* findLeaf(int key) const {
        const Node* n = root_;
        while (!n->leaf) n = n->children[upperIdx(n->keys, key)];
        return n;
    }

    const Node* leftmostLeaf() const {
        const Node* n = root_;
        while (!n->leaf) n = n->children[0];
        return n;
    }

    // Inserts into the subtree at `node`. Returns true if `node` split in two;
    // then upKey is the separator to push into the parent and newRight is the new right half.
    bool insertRec(Node* node, int key, int value, int& upKey, Node*& newRight, bool& inserted) {
        if (node->leaf) {
            int i = lowerIdx(node->keys, key);
            if (i < static_cast<int>(node->keys.size()) && node->keys[i] == key) {
                node->values[i] = value;            // key exists: just update
                inserted = false;
                return false;
            }
            node->keys.insert(node->keys.begin() + i, key);
            node->values.insert(node->values.begin() + i, value);
            inserted = true;
            if (static_cast<int>(node->keys.size()) <= maxKeys_) return false;

            // Leaf overflow: split. The right half's first key is COPIED up
            // (it also stays in the leaf, because leaves hold all the data).
            int mid = static_cast<int>(node->keys.size()) / 2;
            Node* right = new Node(true);
            right->keys.assign(node->keys.begin() + mid, node->keys.end());
            right->values.assign(node->values.begin() + mid, node->values.end());
            node->keys.resize(mid);
            node->values.resize(mid);
            right->next = node->next;               // keep the leaf chain intact
            node->next = right;
            upKey = right->keys[0];
            newRight = right;
            return true;
        }

        int idx = upperIdx(node->keys, key);        // which child holds this key's range
        int childUp = 0;
        Node* childRight = nullptr;
        bool childSplit = insertRec(node->children[idx], key, value, childUp, childRight, inserted);
        if (!childSplit) return false;

        // The child split: add its separator and new right sibling here.
        node->keys.insert(node->keys.begin() + idx, childUp);
        node->children.insert(node->children.begin() + idx + 1, childRight);
        if (static_cast<int>(node->keys.size()) <= maxKeys_) return false;

        // Internal overflow: split. The middle key MOVES up (it does not stay in either half).
        int mid = static_cast<int>(node->keys.size()) / 2;
        upKey = node->keys[mid];
        Node* right = new Node(false);
        right->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
        right->children.assign(node->children.begin() + mid + 1, node->children.end());
        node->keys.resize(mid);
        node->children.resize(mid + 1);
        newRight = right;
        return true;
    }

    int minKeys() const { return maxKeys_ / 2; }

    // Removes `key` from the subtree at `node`; sets `removed` if it was found.
    // If the child we descended into ends up too small, repair it before returning.
    void eraseRec(Node* node, int key, bool& removed) {
        if (node->leaf) {
            int i = lowerIdx(node->keys, key);
            if (i < static_cast<int>(node->keys.size()) && node->keys[i] == key) {
                node->keys.erase(node->keys.begin() + i);
                node->values.erase(node->values.begin() + i);
                removed = true;
            }
            return;
        }
        int idx = upperIdx(node->keys, key);
        eraseRec(node->children[idx], key, removed);
        if (removed && static_cast<int>(node->children[idx]->keys.size()) < minKeys())
            fixUnderflow(node, idx);
    }

    // parent->children[idx] has too few keys: borrow from a sibling, or merge.
    void fixUnderflow(Node* parent, int idx) {
        Node* child = parent->children[idx];
        Node* left = (idx > 0) ? parent->children[idx - 1] : nullptr;
        Node* right = (idx + 1 < static_cast<int>(parent->children.size()))
                          ? parent->children[idx + 1] : nullptr;

        if (left != nullptr && static_cast<int>(left->keys.size()) > minKeys()) {
            // Borrow the left sibling's LAST entry.
            if (child->leaf) {
                child->keys.insert(child->keys.begin(), left->keys.back());
                child->values.insert(child->values.begin(), left->values.back());
                left->keys.pop_back();
                left->values.pop_back();
                parent->keys[idx - 1] = child->keys[0];
            } else {
                // Internal nodes rotate through the parent: the separator comes down,
                // the sibling's last key goes up.
                child->keys.insert(child->keys.begin(), parent->keys[idx - 1]);
                child->children.insert(child->children.begin(), left->children.back());
                parent->keys[idx - 1] = left->keys.back();
                left->keys.pop_back();
                left->children.pop_back();
            }
        } else if (right != nullptr && static_cast<int>(right->keys.size()) > minKeys()) {
            // Borrow the right sibling's FIRST entry.
            if (child->leaf) {
                child->keys.push_back(right->keys.front());
                child->values.push_back(right->values.front());
                right->keys.erase(right->keys.begin());
                right->values.erase(right->values.begin());
                parent->keys[idx] = right->keys[0];
            } else {
                child->keys.push_back(parent->keys[idx]);
                child->children.push_back(right->children.front());
                parent->keys[idx] = right->keys.front();
                right->keys.erase(right->keys.begin());
                right->children.erase(right->children.begin());
            }
        } else if (left != nullptr) {
            mergeChildren(parent, idx - 1);     // fold this child into its left sibling
        } else {
            mergeChildren(parent, idx);         // no left sibling: fold the right one into this child
        }
    }

    // Merges parent->children[s + 1] into parent->children[s] and removes
    // the separator between them from the parent.
    void mergeChildren(Node* parent, int s) {
        Node* l = parent->children[s];
        Node* r = parent->children[s + 1];
        if (l->leaf) {
            l->keys.insert(l->keys.end(), r->keys.begin(), r->keys.end());
            l->values.insert(l->values.end(), r->values.begin(), r->values.end());
            l->next = r->next;                  // unlink r from the leaf chain
        } else {
            l->keys.push_back(parent->keys[s]); // the separator comes down between the two halves
            l->keys.insert(l->keys.end(), r->keys.begin(), r->keys.end());
            l->children.insert(l->children.end(), r->children.begin(), r->children.end());
        }
        parent->keys.erase(parent->keys.begin() + s);
        parent->children.erase(parent->children.begin() + s + 1);
        delete r;
    }

    // Checks one node, then recurses. All keys in this subtree must lie in [lo, hi).
    bool validateNode(const Node* n, long long lo, long long hi, int depth,
                      int& leafDepth, long long& count, bool isRoot) const {
        const int k = static_cast<int>(n->keys.size());
        if (k > maxKeys_) return false;
        if (!isRoot && k < maxKeys_ / 2) return false;
        for (int i = 0; i < k; ++i) {
            if (n->keys[i] < lo || n->keys[i] >= hi) return false;
            if (i > 0 && n->keys[i] <= n->keys[i - 1]) return false;
        }

        if (n->leaf) {
            if (static_cast<int>(n->values.size()) != k) return false;
            if (leafDepth == -1) leafDepth = depth;
            if (leafDepth != depth) return false;          // all leaves at the same depth
            count += k;
            return true;
        }

        if (k < 1) return false;                           // an internal node needs a separator
        if (static_cast<int>(n->children.size()) != k + 1) return false;
        for (int i = 0; i <= k; ++i) {
            long long childLo = (i == 0) ? lo : n->keys[i - 1];
            long long childHi = (i == k) ? hi : n->keys[i];
            if (!validateNode(n->children[i], childLo, childHi, depth + 1, leafDepth, count, false))
                return false;
        }
        return true;
    }
};
