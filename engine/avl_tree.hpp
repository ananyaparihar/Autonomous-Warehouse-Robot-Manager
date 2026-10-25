// avl_tree.hpp - AVL tree (self-balancing binary search tree) with traversals.
//
// Warehouse meaning: an ordered index for the data you ask about in order, for
// example orders by arrival time or stock by SKU. Key = int, value = int.
//
// A plain binary search tree degrades to a linked list if keys arrive sorted
// (every lookup then costs O(n)). An AVL tree prevents that with one rule:
//
//     for every node, the heights of its left and right subtrees differ by at most 1.
//
// When an insert or delete breaks the rule, the tree repairs itself with
// rotations (local re-wiring that keeps the sorted order):
//
//     Left-Left case   -> one right rotation
//     Right-Right case -> one left rotation
//     Left-Right case  -> left rotation on the child, then right rotation
//     Right-Left case  -> right rotation on the child, then left rotation
//
// Complexity (n keys):
//   find / insert / erase: O(log n)   (height is at most about 1.44 * log2(n))
//   traversals: O(n)
//   range query returning r results: O(log n + r)
//   space: O(n)
//
// The rotation counter is there so you can report how much re-balancing costs.
// Written to compile with older compilers (C++11).

#pragma once
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <queue>
#include <utility>
#include <vector>

class AvlTree {
public:
    AvlTree() : root_(nullptr), size_(0), comparisons_(0), rotations_(0) {}
    ~AvlTree() { destroy(root_); }

    AvlTree(const AvlTree&) = delete;
    AvlTree& operator=(const AvlTree&) = delete;

    // Adds the key, or overwrites its value if it exists.
    // Returns true if the key was new.
    bool insert(int key, int value) {
        bool inserted = false;
        root_ = insertRec(root_, key, value, inserted);
        if (inserted) ++size_;
        return inserted;
    }

    // Removes the key. Returns true if it was there.
    bool erase(int key) {
        bool removed = false;
        root_ = eraseRec(root_, key, removed);
        if (removed) --size_;
        return removed;
    }

    bool find(int key, int& value) const {
        const Node* n = root_;
        while (n != nullptr) {
            ++comparisons_;
            if (key < n->key) n = n->left;
            else if (key > n->key) n = n->right;
            else { value = n->value; return true; }
        }
        return false;
    }

    bool contains(int key) const {
        int ignored = 0;
        return find(key, ignored);
    }

    long long size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // Number of levels. An empty tree has height 0, a single node height 1.
    int height() const { return heightOf(root_); }

    // ---- Traversals (each visits all n nodes in O(n)) ----

    // Left, node, right: keys come out in SORTED order.
    std::vector<int> inorder() const {
        std::vector<int> out;
        inorderRec(root_, out);
        return out;
    }

    // Node, left, right: a node comes before its children (useful for copying a tree).
    std::vector<int> preorder() const {
        std::vector<int> out;
        preorderRec(root_, out);
        return out;
    }

    // Left, right, node: children come before the node (useful for deleting a tree).
    std::vector<int> postorder() const {
        std::vector<int> out;
        postorderRec(root_, out);
        return out;
    }

    // Level by level, top to bottom (breadth-first). One inner vector per level.
    std::vector<std::vector<int> > levelOrder() const {
        std::vector<std::vector<int> > levels;
        if (root_ == nullptr) return levels;
        std::queue<const Node*> q;
        q.push(root_);
        while (!q.empty()) {
            std::size_t width = q.size();
            std::vector<int> level;
            for (std::size_t i = 0; i < width; ++i) {
                const Node* n = q.front();
                q.pop();
                level.push_back(n->key);
                if (n->left != nullptr) q.push(n->left);
                if (n->right != nullptr) q.push(n->right);
            }
            levels.push_back(level);
        }
        return levels;
    }

    // All (key, value) pairs in sorted order.
    std::vector<std::pair<int, int> > toVector() const {
        std::vector<std::pair<int, int> > out;
        pairsRec(root_, INT_MIN, INT_MAX, true, true, out);
        return out;
    }

    // All pairs with lo <= key <= hi, sorted. Skips subtrees that cannot contain matches.
    std::vector<std::pair<int, int> > range(int lo, int hi) const {
        std::vector<std::pair<int, int> > out;
        if (lo > hi) return out;
        pairsRec(root_, lo, hi, false, false, out);
        return out;
    }

    // ---- Instrumentation for the report ----
    long long comparisons() const { return comparisons_; }
    long long rotations() const { return rotations_; }
    void resetCounters() { comparisons_ = 0; rotations_ = 0; }

    // Checks: sorted order (BST rule), stored heights are right, and every
    // node is balanced (difference of at most 1), and the node count matches.
    bool validate() const {
        bool ok = true;
        long long count = 0;
        validateNode(root_, LLONG_MIN, LLONG_MAX, count, ok);
        return ok && count == size_;
    }

private:
    struct Node {
        int key;
        int value;
        int height;          // levels in the subtree rooted here (a leaf has height 1)
        Node* left;
        Node* right;
        Node(int k, int v) : key(k), value(v), height(1), left(nullptr), right(nullptr) {}
    };

    Node* root_;
    long long size_;
    mutable long long comparisons_;
    long long rotations_;

    static void destroy(Node* n) {
        if (n == nullptr) return;
        destroy(n->left);
        destroy(n->right);
        delete n;
    }

    static int heightOf(const Node* n) { return n == nullptr ? 0 : n->height; }

    // Positive: left side is taller. Negative: right side is taller.
    static int balanceOf(const Node* n) {
        return n == nullptr ? 0 : heightOf(n->left) - heightOf(n->right);
    }

    static void updateHeight(Node* n) {
        n->height = 1 + std::max(heightOf(n->left), heightOf(n->right));
    }

    /*       y                x
     *      / \              / \
     *     x   C    --->    A   y
     *    / \                  / \
     *   A   B                B   C
     */
    Node* rotateRight(Node* y) {
        Node* x = y->left;
        y->left = x->right;
        x->right = y;
        updateHeight(y);      // y is now lower, so update it first
        updateHeight(x);
        ++rotations_;
        return x;             // x is the new root of this subtree
    }

    /*     x                    y
     *    / \                  / \
     *   A   y      --->      x   C
     *      / \              / \
     *     B   C            A   B
     */
    Node* rotateLeft(Node* x) {
        Node* y = x->right;
        x->right = y->left;
        y->left = x;
        updateHeight(x);
        updateHeight(y);
        ++rotations_;
        return y;
    }

    // Recomputes the height and, if the node is out of balance, rotates.
    // Returns the (possibly new) root of this subtree.
    Node* rebalance(Node* n) {
        updateHeight(n);
        int balance = balanceOf(n);
        if (balance > 1) {                              // left side too tall
            if (balanceOf(n->left) < 0)                 // Left-Right case: fix the child first
                n->left = rotateLeft(n->left);
            return rotateRight(n);                      // Left-Left case
        }
        if (balance < -1) {                             // right side too tall
            if (balanceOf(n->right) > 0)                // Right-Left case
                n->right = rotateRight(n->right);
            return rotateLeft(n);                       // Right-Right case
        }
        return n;
    }

    Node* insertRec(Node* n, int key, int value, bool& inserted) {
        if (n == nullptr) {
            inserted = true;
            return new Node(key, value);
        }
        ++comparisons_;
        if (key < n->key) n->left = insertRec(n->left, key, value, inserted);
        else if (key > n->key) n->right = insertRec(n->right, key, value, inserted);
        else { n->value = value; return n; }            // key exists: update, no shape change
        return inserted ? rebalance(n) : n;
    }

    Node* eraseRec(Node* n, int key, bool& removed) {
        if (n == nullptr) return nullptr;               // key not in the tree
        ++comparisons_;
        if (key < n->key) {
            n->left = eraseRec(n->left, key, removed);
        } else if (key > n->key) {
            n->right = eraseRec(n->right, key, removed);
        } else {
            removed = true;
            if (n->left == nullptr || n->right == nullptr) {
                // Zero or one child: the child (or nothing) takes this node's place.
                Node* child = (n->left != nullptr) ? n->left : n->right;
                delete n;
                return child;
            }
            // Two children: copy in the in-order successor (smallest key in the
            // right subtree), then delete that successor from the right subtree.
            Node* s = n->right;
            while (s->left != nullptr) s = s->left;
            n->key = s->key;
            n->value = s->value;
            bool ignored = false;
            n->right = eraseRec(n->right, s->key, ignored);
        }
        return removed ? rebalance(n) : n;
    }

    static void inorderRec(const Node* n, std::vector<int>& out) {
        if (n == nullptr) return;
        inorderRec(n->left, out);
        out.push_back(n->key);
        inorderRec(n->right, out);
    }

    static void preorderRec(const Node* n, std::vector<int>& out) {
        if (n == nullptr) return;
        out.push_back(n->key);
        preorderRec(n->left, out);
        preorderRec(n->right, out);
    }

    static void postorderRec(const Node* n, std::vector<int>& out) {
        if (n == nullptr) return;
        postorderRec(n->left, out);
        postorderRec(n->right, out);
        out.push_back(n->key);
    }

    // In-order walk that only enters subtrees which can hold keys in [lo, hi].
    // unboundedLo / unboundedHi = true means "no limit on that side" (used by toVector).
    static void pairsRec(const Node* n, int lo, int hi, bool unboundedLo, bool unboundedHi,
                         std::vector<std::pair<int, int> >& out) {
        if (n == nullptr) return;
        if (unboundedLo || lo < n->key) pairsRec(n->left, lo, hi, unboundedLo, unboundedHi, out);
        if ((unboundedLo || n->key >= lo) && (unboundedHi || n->key <= hi))
            out.push_back(std::make_pair(n->key, n->value));
        if (unboundedHi || n->key < hi) pairsRec(n->right, lo, hi, unboundedLo, unboundedHi, out);
    }

    // Returns the real height of the subtree; clears `ok` on any violation.
    // All keys in this subtree must lie strictly between lo and hi.
    int validateNode(const Node* n, long long lo, long long hi, long long& count, bool& ok) const {
        if (n == nullptr) return 0;
        if (n->key <= lo || n->key >= hi) ok = false;
        ++count;
        int lh = validateNode(n->left, lo, n->key, count, ok);
        int rh = validateNode(n->right, n->key, hi, count, ok);
        if (std::abs(lh - rh) > 1) ok = false;           // the AVL rule
        int h = 1 + std::max(lh, rh);
        if (n->height != h) ok = false;                  // stored heights must be correct
        return h;
    }
};
