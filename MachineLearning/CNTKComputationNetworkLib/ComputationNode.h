//
// <copyright file="ComputationNode.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include "Basics.h"
#include "Matrix.h"
#include "ScriptableObjects.h"
#include "Sequences.h"
#include "MatrixPool.h"

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

//#define RNN_DEBUG 1
#define DEFAULT_HIDDEN_ACTIVATION 0.1

#ifndef NOT_IMPLEMENTED
#define NOT_IMPLEMENTED \
{   \
    fprintf(stderr, "Inside File: %s  Line: %d  Function: %s  -> Feature Not Implemented.\n", __FILE__, __LINE__, __FUNCTION__); \
    LogicError("Not Implemented"); \
}
#endif

#pragma warning (disable: 4267)

//version number to control how to read and write 
#define CNTK_MODEL_VERSION_1 1
#define CNTK_MODEL_VERSION_2 2
#define CURRENT_CNTK_MODEL_VERSION 2

namespace Microsoft { namespace MSR { namespace CNTK {

    enum CopyNodeFlags
    {
        copyNodeNull = 0,               // invalid value
        copyNodeValue=1,                // copy everything but the children links
        copyNodeChildren=2,             // only copy over children links
        copyNodeAll=3,                  // copy everything
        copyNodeChildrenCrossNetwork=4, // allow a cross network child copy
    };

    // describes inner layout of feature vector that is an image
    // TODO: This will grow into a more general tensor mechanism.
    // TODO: SaveToFile() and LoadFromFile() currenrly use individual elements; provide an overload for the entire object.
    struct ImageLayout
    {
        size_t width, height, channels;
        // BUGBUG: This initialization is not correct. This must match GetNumRows(). We probably cannot have all three members here.
        // Idea: We could construct this thing with a ref to the enclosing ComputationNode, and replace 'width' by an expression.
        ImageLayout() : width(1), height(1), channels(1) { }
        ImageLayout(size_t width, size_t height, size_t channels) : width(width), height(height), channels(channels) { }
        //void Set(size_t width, size_t height, size_t channels) { this->width = width; this->height = height; this->channels = channels; }
        void Invalidate() { width = SIZE_MAX; height = SIZE_MAX; channels = SIZE_MAX; } // TODO: clean up the valid/invalid situation (this is currently done inconsistently)
        size_t GetNumElements() const { return width * height * channels; }
        bool operator==(const ImageLayout & other) const { return width == other.width && height == other.height &&channels == other.channels; }
    };

#pragma region base computation class

    // =======================================================================
    // IComputationNode -- set of methods that are to be implemented (or optionally overridable) by node implementations.
    // =======================================================================

    class ComputationNodeBase;
    struct/*interface*/ IComputationNode
    {
        typedef shared_ptr<ComputationNodeBase> ComputationNodeBasePtr;

        // --- these must be implemented by each node

        virtual ComputationNodeBase * NewThis(DEVICEID_TYPE deviceId, const wstring & name) = 0;
        // TODO: OperationName calls static TypeName which does not match the actual type names in that the 'Node' is missing.
        virtual const std::wstring OperationName() const = 0;
#define OperationNameOf(T) (T<float>::TypeName())    // we are templated, but for this the type param matters not. So we just pick one, and hide that fact.

        virtual void OnEvaluateBeginIteration() = 0;
        virtual void EvaluateThisNode(const FrameRange &) = 0;  // forward prop for one minibatch
        virtual void OnEvaluateEndIteration() = 0;              // called after last iteration step of EvaluateThisNode()
        virtual void ComputeInputPartial(const size_t inputIndex, const FrameRange &) = 0;
        virtual void ComputeInputPartial(const size_t inputIndex) = 0;   // TODO: this will be replaced by FrameRange version

        // --- optional overrides

        // Any override must call Base version as well.
        // Default implementations are in ComputationNodeBase or ComputationNode<ElemType>.
        // TODO: is this always just called with deviceId == m_deviceId?

        virtual void Validate(bool isFinalValidationPass) = 0;          // main base validation function
        virtual void InferImageDimsFromInputs() = 0;
        virtual size_t UpdateFunctionAndGradientMBSize(size_t numCols = SIZE_MAX/*means take from layout--this is the main use*/) = 0;
        virtual void SaveToFile(File& fstream) const = 0;
        virtual void LoadFromFile(File& /*fstream*/, size_t /*modelVersion*/) = 0;
        virtual void CopyTo(const ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const = 0;
        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId) = 0;
        virtual void PrintSelfBeforeValidation() const = 0;             // called in validation loop right before Validate()
        virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const = 0;
        virtual bool RequiresPreCompute() const = 0;                    // return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() = 0; // // indicates whether special handling is needed.The standard handleing will be just mask the function values after the evalaution and mask the gradient before gradiant computation for the children. this is not valid for all criterion nodes whose result is a scalar.

        // --- left-overs from refactoring--keeping it here for a while for debugging

        // return true if the node's value should be computed in batch mode only, e.g., time-reverse node
        //virtual bool RequiresBatchMode() const { return false; }  // not used actually
    };

    // =======================================================================
    // ComputationNetworkOwnedNodeState -- class to collect ComputationNode members that are really owned by ComputationNetwork
    // These members are only to be set, changed, and read by ComputationNetwork code.
    // TODO: We could go much further and move all network-level evaluation routines into here as well.
    //       I won't do it now as it will create a massive diff that would make merging of other ongoing changes much harder.
    // =======================================================================

    class ComputationNetwork;
    struct ComputationNetworkOwnedNodeState
    {
        friend class ComputationNetwork;

        ComputationNetworkOwnedNodeState() :
            m_needsGradient(false),
            m_loopId(-1),
            m_visitedOrder(-1),
            m_index(-1),
            m_lowLink(-1),
            m_indexInLoop(0),
            m_visited(false),
            m_inStack(false)
        { }

        void ClearCache()
        {
            m_loopId = -1;
            m_visitedOrder = -1;
            m_index = -1;
            m_lowLink = -1;
            m_indexInLoop = 0;
            m_visited = false;
            m_inStack = false;
        }

        void SetLoopId(const int id) { m_loopId = id; }
        int GetLoopId() const { return m_loopId; }

        void SetVisitedOrder(const int id) { m_visitedOrder = id; }
        size_t GetVisitedOrder() const { return m_visitedOrder; }

        void SetIndex(const size_t ind) { m_index = ind; }
        size_t GetIndex() const { return m_index; }

        void SetLowLink(const size_t lowlink) { m_lowLink = lowlink; }
        size_t GetLowLink() const { return m_lowLink; }

        void SetVisited(const bool visited) { m_visited = visited; }
        bool IsVisisted() const { return m_visited; }

        void SetInStack(const bool instack) { m_inStack = instack; }
        bool IsInStack() const { return m_inStack; }

        void SetIndexInLoop(const size_t index) { m_indexInLoop = index; }
        size_t GetIndexInLoop() const { return m_indexInLoop; }

        void InitRecurrentNode()    // this initialization says that this node is not inside a loop
        {
            SetLoop(false);
        }

        bool HasLoop() const { return m_hasloop; }
        void SetLoop(bool hasLoop) { m_hasloop = hasLoop; }

        void CopyTo(ComputationNetworkOwnedNodeState & other) const
        {
            // TODO: is that really all we copy? (this is a result of refactoring, so it seems yes indeed). Should we at least ClearCache()?
            other.m_evalTimeStamp = m_evalTimeStamp;
            other.m_hasloop = m_hasloop;
            other.m_needsGradient = m_needsGradient;
        }

        int64_t UpdateEvalTimeStamp()
        {
            m_evalTimeStamp = atomic_fetch_add(&s_timeStampCounter, (unsigned long long int) 1);    // TODO: does this really need to be atomic? We are not multi-threaded
            return m_evalTimeStamp;
        }

        void ResetEvalTimeStamp()
        {
            m_evalTimeStamp = s_timeStampCounter;
        }

        int64_t GetEvalTimeStamp() const { return m_evalTimeStamp; }

        int64_t CreateUniqId() const
        {
            return atomic_fetch_add(&s_timeStampCounter, (unsigned long long int) 1);
        }

        static bool IsSmaller(const ComputationNetworkOwnedNodeState * lhs, const ComputationNetworkOwnedNodeState * rhs)
        {
            return lhs->m_visitedOrder < rhs->m_visitedOrder;
        }

    private:

        static atomic_ullong s_timeStampCounter;
        int64_t m_evalTimeStamp; //this is used to reduce unnecessary recomputation when a different node in the model is reevaluated

        // for loop nodes
        bool m_hasloop;
        int m_loopId;           // index into recurrent info array (TODO: verify this)

    protected:  // TODO: should be fully encapsulated here
        bool m_needsGradient;   // true if this node or any children need a gradient to be computed (for own consumption or propagation to somewhere in the child tree)
    private:

        // the order in reverse graph. 
        int m_visitedOrder;
        int m_index;
        int m_lowLink;          // TODO: comment this, as it is not obvious
        bool m_visited;
        bool m_inStack;
        int m_indexInLoop;
    };

    // =======================================================================
    // ComputationNodeBase -- abstract base class for all computation nodes
    // TODO: decide the name. This does contain actual members such as the node name, so it's not really a pure interface.
    // =======================================================================

    class ComputationNodeBase :
        public IComputationNode,
        public/*protected*/ ComputationNetworkOwnedNodeState,  // TODO: figure this out, somehow the 'friend' thing does not work
        public ScriptableObjects::ComputationNodeObject,
        public ScriptableObjects::WithTag, public ScriptableObjects::HasName, public ScriptableObjects::HasToString,
        public std::enable_shared_from_this<ComputationNodeBase>
    {
        // note: enable_shared_from_this<> allows to create a shared_ptr from a raw pointer to this that is correctly aware of all other shared_ptrs (same ref count)
    public:
        typedef shared_ptr<ComputationNodeBase> ComputationNodeBasePtr;

        ComputationNodeBase(DEVICEID_TYPE deviceId, const wstring & name) :
            m_deviceId(deviceId),
            m_parameterUpdateRequired(false),
            m_nodeName(name == L"" ? CreateUniqNodeName() : name)
        {
        }
        virtual ~ComputationNodeBase(){}

        virtual void /*IComputationNode::*/CopyTo(const ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            if (OperationName() != node->OperationName())
                RuntimeError("Cannot copy from one node type to another node type");
            if (flags & CopyNodeFlags::copyNodeChildren)
            {
                node->m_children = m_children;
            }
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_deviceId = m_deviceId;
                node->m_parameterUpdateRequired = m_parameterUpdateRequired;
                node->m_nodeName = newName;

                node->m_inputImageLayout = m_inputImageLayout;
                node->m_outputImageLayout = m_outputImageLayout;

                ComputationNetworkOwnedNodeState::CopyTo(*node);
            }
        }

        virtual ComputationNodeBasePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) = 0;

        // TODO: make sure this does not get implemented in any of the base classes
        DEVICEID_TYPE GetDeviceId() const { return m_deviceId; }    // TODO: remove, only used from copy constructor which will go away

        virtual void SaveToFile(File& fstream) const
        {
            fstream << OperationName() << NodeName();
        }

        virtual void LoadFromFile(File& /*fstream*/, size_t /*modelVersion*/)
        {
            // it is assumed that OperationName and NodeName have already been consumed--some asymmetry between Save and Load
            // base class has nothing to load
        }

        // float/double-independent access to the m_functionValues for a few specific use cases
        // TODO: Not nice. This would go away if we abstracted out the matrix type as well from float/double.
        virtual size_t GetNumRows() const = 0;
        virtual size_t GetNumCols() const = 0;
        pair<size_t, size_t> GetDims() { return make_pair(GetNumRows(), GetNumCols()); }
        virtual void Resize(size_t rows, size_t cols) = 0;
        virtual void Resize(ComputationNodeBasePtr node) { Resize(node->GetNumRows(), node->GetNumCols()); }
        void VerifySize(size_t rows, size_t cols)
        {
            if (rows != GetNumRows() || cols != GetNumCols())
                LogicError("VerifySize: expected m_functionValues size %d x %d, but it is %d x %d",
                           (int)rows, (int)cols, (int)GetNumRows(), (int)GetNumCols());
        }
        virtual void VerifySize(ComputationNodeBasePtr node) { VerifySize(node->GetNumRows(), node->GetNumCols()); }
        virtual double Get00Element() const = 0;

        // validation
        virtual void Validate(bool isFinalValidationPass)           // main base validation function
        {
            // check for NULL pointers
            for (size_t i = 0; i < m_children.size(); i++)
            {
                if (!m_children[i])
                    RuntimeError("Validate: Input [%d] of %ls node '%ls' is empty (NULL, not connected).", (int)i, OperationName().c_str(), NodeName().c_str());
            }
            // check for empty inputs
            if (isFinalValidationPass)
            {
                for (const auto & child : m_children)
                {
                    if (child->GetNumRows() == 0 || (!child->HasMBLayout() && child->GetNumCols() == 0))
                        RuntimeError("%ls %ls operation: input %ls %ls has 0 elements.",
                                     NodeName().c_str(), OperationName().c_str(), child->NodeName().c_str(), child->OperationName().c_str());
                }
            }
        }
        // helper functions for common cases
    private:
        ComputationNodeBasePtr Inputs(size_t index) const { return m_children[index]; } // TODO: delete this; change to m_children
        // determine number of columns from a child and/or layout
        size_t DetermineNumCols(const ComputationNodeBasePtr & child) const
        {
            size_t childCols = child->GetNumCols();     // this is what the child says
            if (!m_pMBLayout)                           // no layout: copy from child
                return childCols;
            size_t cols = m_pMBLayout->GetNumCols();    // layout: get it from there, but validate against child
            if (childCols != cols)
                RuntimeError("%ls %ls operation: ");
            return cols;
        }
    protected:
        void ValidateUnaryMap(bool isFinalValidationPass);
        void ValidateUnaryReduce(bool isFinalValidationPass);
        void ValidateInferBinaryChildrenDims();
        void ValidateBinaryZip(bool isFinalValidationPass, bool allowMultiples);
        void ValidateBinaryReduce(bool isFinalValidationPass);
    public:

        virtual bool UnitTest() { return true; }

        virtual void AttachInputs(const std::vector<ComputationNodeBasePtr>& inputs) = 0;
        // convenience versions that take individual arguments
        void AttachInputs(const ComputationNodeBasePtr singleInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { singleInput } ); }
        void AttachInputs(const ComputationNodeBasePtr leftInput, const ComputationNodeBasePtr rightInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { leftInput, rightInput } ); }
        void AttachInputs(const ComputationNodeBasePtr leftInput, const ComputationNodeBasePtr middleInput, const ComputationNodeBasePtr rightInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { leftInput, middleInput, rightInput } ); }
        void AttachInputs(const ComputationNodeBasePtr firstInput, const ComputationNodeBasePtr secondInput, const ComputationNodeBasePtr thirdInput, const ComputationNodeBasePtr fourthInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { firstInput, secondInput, thirdInput, fourthInput } ); }
        void AttachInputs(const ComputationNodeBasePtr firstInput, const ComputationNodeBasePtr secondInput, const ComputationNodeBasePtr thirdInput, const ComputationNodeBasePtr fourthInput, const ComputationNodeBasePtr fifthInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { firstInput, secondInput, thirdInput, fourthInput, fifthInput } ); }
        void AttachInputs(const ComputationNodeBasePtr firstInput, const ComputationNodeBasePtr secondInput, const ComputationNodeBasePtr thirdInput, const ComputationNodeBasePtr fourthInput, const ComputationNodeBasePtr fifthInput, const ComputationNodeBasePtr sixthInput) { AttachInputs(std::vector<ComputationNodeBasePtr> { firstInput, secondInput, thirdInput, fourthInput, fifthInput, sixthInput } ); }

        virtual void DetachInputs() { m_children.clear(); }

        const std::vector<ComputationNodeBasePtr> & GetChildren() const { return m_children; }

        //return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.
        virtual bool /*IComputationNode::*/RequiresPreCompute() const { return false; }

        /*HasName::*/void SetName(const std::wstring & newName) // also for use by ExperimentalNetworkBuilder
        {
            m_nodeName = newName;
            fprintf(stderr, "Node --> %ls = %ls\n", NodeName().c_str(), OperationName().c_str()), fflush(stderr);
        }

        void LinkToMBLayout(MBLayoutPtr pMBLayout) { m_pMBLayout = pMBLayout; }
        MBLayoutPtr GetMBLayout() { return m_pMBLayout; }
        bool HasMBLayout() const { return !!m_pMBLayout; }

        std::wstring GetName() const { return m_nodeName; }

        // temporary function that is called to verify stuff is called as I think it is. Delete if this does not fire for a while.
        void VerifyNumParallelSequences(size_t bsz)
        {
            if (bsz != m_pMBLayout->GetNumParallelSequences())
                LogicError("VerifyNumParallelSequences: value inconsistent with MB layout");
        }

    protected:
    public: // the following should be protected, but nodes inquire about their children, requiring public access
        // This is used at 284 places inside nodes, most of the time as
        // ...Slice(frameRange/*TODO: delete this:*/.Check_t(GetNumParallelSequences()), m_pMBLayout)
        size_t GetNumParallelSequences() const
        {
#if 1
            if (!m_pMBLayout)       // TODO: temporary workaround to Check_t() calls which call this. TODO: Delete the first arg from Check_t() after memshare merge.
                return SIZE_MAX;
#endif
            return m_pMBLayout->GetNumParallelSequences();
        }

        // get our current number of time steps for this node
        // This inquires the MB layout.
        size_t GetNumTimeSteps() const
        {
            if (!m_pMBLayout)
                LogicError("GetNumTimeSteps: invalid to call on a node without MB layout"); // since it has no notion of time
                //return GetNumCols();
#if 0       // can't check here; this is sometimes inquired as part of the process of setting the right #cols
            if (m_pMBLayout->GetNumTimeSteps() * m_pMBLayout->GetNumParallelSequences() != GetNumCols())
            {
                // TODO: remove this fprintf() once it no longer triggers
                fprintf(stderr, "GetNumTimeSteps: inconsistency between layout and actual number of columns for node '%ls', seq=%d x T=%d vs. cols=%d\n",
                        NodeName().c_str(), (int)m_pMBLayout->GetNumParallelSequences(), (int)m_pMBLayout->GetNumTimeSteps(), (int)GetNumCols());
                LogicError("GetNumTimeSteps: inconsistency between layout and actual number of columns for node '%ls', seq=%d x T=%d vs. cols=%d",
                           NodeName().c_str(), (int)m_pMBLayout->GetNumParallelSequences(), (int)m_pMBLayout->GetNumTimeSteps(), (int)GetNumCols());
            }
            // TODO: ^^ much of this should go away, as in the future, the layout will always correctly know the #samples
#endif
            return m_pMBLayout->GetNumTimeSteps();
        }
    public:

        // implemented by ComputationNode<ElemType>
        // for debugging purpose
        virtual void PrintSelf(bool printMatrices = false) const = 0;

        // called in validation loop right before Validate()
        virtual void /*IComputationNode::*/PrintSelfBeforeValidation() const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");
                for (size_t i = 0; i<ChildrenSize(); i++)
                {
                    const auto & child = m_children[i];
                    if (i > 0)
                        fprintf(stderr, ", ");

                    if (child == nullptr)
                    {
                        fprintf(stderr, "NULL");
                        continue;
                    }

                    const char * mbSizeMark = child->m_pMBLayout ? "MBSize " : "";
                    if (IsChildAnImage(i))  //image
                        fprintf(stderr, "%ls[%lu {W=%lu, H=%lu, C=%lu}, %s%lu]", child->NodeName().c_str(), child->GetNumRows(),
                                child->m_outputImageLayout.width, child->m_outputImageLayout.height, child->m_outputImageLayout.channels, mbSizeMark, child->GetNumCols());
                    else
                        fprintf(stderr, "%ls[%lu, %s%lu]", child->NodeName().c_str(), child->GetNumRows(), mbSizeMark, child->GetNumCols());
                }
                fprintf(stderr, ")");
            }
#if 0
            else
            {
                if (m_pMBLayout)
                    fprintf(stderr, "[%lu, MBSize]", GetNumRows());
                else
                    fprintf(stderr, "[%lu, %lu]", GetNumRows(), GetNumCols());
            }
#endif
        }

        const std::wstring& NodeName() const { return m_nodeName; }
        std::wstring& NodeName() { return m_nodeName; }

        bool IsLeaf() const { return ChildrenSize() == 0; }

        void SetParameterUpdateRequired(bool f) { m_parameterUpdateRequired = f; }
        bool IsParameterUpdateRequired() const { return m_parameterUpdateRequired; }

        virtual void /*IComputationNode::*/InferImageDimsFromInputs()
        {
            if (!IsLeaf())
                InferImageDimsFromInput(0); //copy from child 0 by default.
        }

        virtual void ValidateInferChildDims(size_t i, size_t rows, size_t cols) = 0;

        bool IsChildAnImage(const size_t index) const
        {
            return m_children[index]->m_outputImageLayout.width != 1 || m_children[index]->m_outputImageLayout.channels != 1;
        }

        pair<ImageLayout, ImageLayout> GetImageLayouts() const { return make_pair(m_inputImageLayout, m_outputImageLayout); }

        const size_t ChildrenSize() const { return m_children.size(); }     // TODO: rename to NumChildren() or NumInputs(); and inside here where we use m_children, use m_children.size() as well

        virtual void SetInput(const size_t childIndex, const ComputationNodeBasePtr node) = 0;

        virtual void /*IComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange &) = 0;    // (redeclaring, as compiler gets confused otherwise--will go away with ComputeInputPartial(t))
        virtual void ComputeInputPartial(const size_t inputIndex)   // TODO: this will be replaced by FrameRange version
        {
            ComputeInputPartial(inputIndex, FrameRange(/*whole batch*/));      // nodes that do not implement this will know to understand SIZE_MAX as full batch
        }
        virtual void ComputeGradientForChildren() = 0;
        virtual void ComputeGradientForChildren(const size_t timeIdxInSeq) = 0; // TODO: don't we need a FrameRange here, too?

        // masking
        // overridden by <ElemType> variant only
        // TODO: we need a section for those; and ComputationNode<> should mark those as 'final'
        virtual void MaskMissingValuesColumnsToZero() = 0;
        virtual void MaskMissingValuesColumnsToZero(const size_t timeIdxInSeq) = 0; // TODO: change to FrameRange as well
        virtual void MaskMissingGradientColumnsToZero() = 0;
        virtual void MaskMissingGradientColumnsToZero(const size_t timeIdxInSeq) = 0; // TODO: don't we need a FrameRange here, too?

        // indicates whether special handling is needed.The standard handleing will be just mask the function values after the evalaution and mask the gradient before gradiant computation for the children. this is not valid for all criterion nodes whose result is a scalar.
        // overridden to return true by training/eval criteria (and the soon-to-be-deprecated PairNetworkNode, LSTMNode)
        // The need for this seems an artifact of the old inconsistent layout architecture. In the future, this can probably just go away.
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return false; }

        virtual void /*IComputationNode::*/OnEvaluateBeginIteration()             // called before first iteration step of EvaluateThisNode()
        {
            //fprintf(stderr, "OnEvaluateBeginIteration: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
        }
        virtual void /*IComputationNode::*/OnEvaluateEndIteration()               // called after last iteration step of EvaluateThisNode()
        {
            //fprintf(stderr, "OnEvaluateEndIteration: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
        }

    protected:

        void InferImageDimsFromInput(const size_t index, const bool outputSameAsInput = true)
        {
            if (index >= ChildrenSize())
                InvalidArgument("InferImageDimsFromInput: output index");

            const auto & child = m_children[index];
            if (child != nullptr)
                m_inputImageLayout = child->m_outputImageLayout;
            if (outputSameAsInput)
                m_outputImageLayout = m_inputImageLayout;
        }

        void InferMBLayoutFromInputsForStandardCase();

    public:

        static bool IsSmaller(const ComputationNodeBasePtr lhs, const ComputationNodeBasePtr rhs)
        {
            return ComputationNetworkOwnedNodeState::IsSmaller(lhs.get(), rhs.get());
        }

        bool IsEqualTo(const ComputationNodeBasePtr other) const //this will be used to determine whehter two nodes are the same
        {
            if (OperationName() != other->OperationName() || m_children.size() != other->m_children.size())
                return false;

            if (NodeName() == other->NodeName())  //assume names are unique in the system
                return true;

            if (IsLeaf() && other->IsLeaf())  //since names are not equal otherwise will return above
                return false;

            for (size_t i=0; i<m_children.size(); i++)
                if (!(m_children[i] == other->m_children[i]))
                    return false;

            return true;
        }

        // determine enumeration order for everything needed to evaluate this node (and its children)
        // This creates a list such that children are evaluated before their parents.
        // If !forForwardProp then the order will be reversed, suitable for backprop.
        // The 'recurrent' version is only called from FormRecurrentLoops().
        // Side-effects (unbeknownst to the name of the function):
        //  - m_needsGradient flags, are propagated up from children         --BUGBUG! This should only be computed in ValidateSubNetwork().
        //  - m_visitedOrder (only if 'recurrent' flag is set; otherwise leave untouched)
        std::list<ComputationNodeBasePtr> EnumerateNodes(bool forForwardProp/*else get order for backprop*/, bool recurrent)
        {
            std::list<ComputationNodeBasePtr> nodes;
            std::unordered_set<ComputationNodeBasePtr> visited;

            // get forward computation order
            EnumerateNodesR(visited, nodes, recurrent);  // call into the recursive portion of this function below

            // if caller wants order for backprop then reverse it
            if (!forForwardProp)
            {
                assert(!recurrent);     // TODO: not sure if required, but currently only called this way

                // TODO: comment why can't directly reverse(); what's wrong with EnumerateNodes()' result?
                nodes.sort(IsSmaller);  // sort nodes by m_visitedOrder   --TODO: why? What about nodes with visitedOrder -1? Will they stay the same? Comment please!!!
                nodes.reverse();        // and go backwards
            }

            return nodes;
        }
    private:
        // Recursive part of EnumerateNodes().
        void EnumerateNodesR(std::unordered_set<ComputationNodeBasePtr>& visited, std::list<ComputationNodeBasePtr>& result, bool recurrent)
        {
            if (visited.find(shared_from_this()) == visited.end())      // do not include a node twice
            {
                visited.insert(shared_from_this());   // have visited tagged here to avoid infinite loop over children, children's children, etc

                // children first for function evaluation
                if (OperationName() != L"PairNetwork" || !recurrent)    // (don't step through network-pair boundary if recurrent)
                {
                    for (int i = 0; i < m_children.size(); i++)
                    {
                        if (m_children[i])
                            m_children[i]->EnumerateNodesR(visited, result, recurrent);
                    }
                }

                // propagate m_needsGradient flags upwards from leaves
                // TODO: This belongs into Validate().
                if (!IsLeaf())
                    m_needsGradient = ChildrenNeedGradient();  //only nodes that require gradient calculation is included in gradient calculation

                // now that all children are in list before us, put ourselves
                result.push_back(shared_from_this());  //we put this in the list even if it's leaf since we need to use it to determine learnable params 

                if (recurrent)
                    SetVisitedOrder(result.size());
            }
        }
    public:

        std::list<ComputationNodeBasePtr> ReshuffleNodes(std::map<int, std::list<ComputationNodeBasePtr>> recurrentResult)
        {
            std::list<ComputationNodeBasePtr> noRecurrentResult;
            std::unordered_set<ComputationNodeBasePtr> visited;

            ReshuffleNodesForEvalWithRecurrentLoops(visited, recurrentResult, noRecurrentResult);

            return noRecurrentResult;
        }

#if 0
        std::list<ComputationNodeBasePtr> EnumerateNodes(const bool forwardComputation, bool recurrent)
        {
            if (forwardComputation)
            {
                std::list<ComputationNodeBasePtr> result;
                std::unordered_set<ComputationNodeBasePtr> visited;
                EnumerateNodesForEval(visited, result, recurrent);
                return result;
            }
            else
                return EnumerateNodesForGradient();
        }
#endif

    protected:

        bool ChildrenNeedGradient()  const //this is only valid when called in the forward computation order.
        {
            for (int i = 0; i<m_children.size(); i++)
            {
                if (m_children[i] == nullptr)
                    continue;
                if (m_children[i]->m_needsGradient)
                    return true;
            }
            return false;
        }

        // TODO: what does this do?
        // As a side effect, it also propagates m_needsGradient to intermediate nodes
        void ReshuffleNodesForEvalWithRecurrentLoops(std::unordered_set<ComputationNodeBasePtr>& visited, std::map<int, std::list<ComputationNodeBasePtr>>& recurrentResult,
                                                     std::list<ComputationNodeBasePtr>& noRecurrentResult)
        {
            if (visited.find(shared_from_this()) == visited.end())  //not visited
            {
                visited.insert(shared_from_this());   // have visited tagged here to avoid infinite loop over children, children's children, etc

                for (int i = 0; i<m_children.size(); i++)
                    m_children[i]->ReshuffleNodesForEvalWithRecurrentLoops(visited, recurrentResult, noRecurrentResult);

                //children first for function evaluation
                if (!IsLeaf())
                    m_needsGradient = ChildrenNeedGradient();  //only nodes that require gradient calculation is included in gradient calculation

                if (GetLoopId() >= 0)
                    recurrentResult[GetLoopId()].push_back(shared_from_this());
                else
                    noRecurrentResult.push_back(shared_from_this());  //we put this in the list even if it's leaf since we need to use it to determine learnable params 
            }
        }

    public:

        // check whether a node is up-to-date w.r.t. its children, for lazy evaluation
        // If this returns false, node must be evaluated to update m_functionValues.
        bool IsFuncValueOlderThanInputs() const
        {
            for (size_t i = 0; i<ChildrenSize(); i++)
            {
                //the second condition is used when the time stamp change from positive to negative
                if (m_children[i]->GetEvalTimeStamp() >= GetEvalTimeStamp() || m_children[i]->GetEvalTimeStamp() + 1e10 < GetEvalTimeStamp())
                    return true;
            }

            return false;
        }

        virtual void ClearGradientForChildren(const int /*iActMiniBatchSize*/) = 0;

        typedef std::pair<ComputationNodeBasePtr, ComputationNodeBasePtr> ComputationArc;
        // [1/13/2015 erw] add to enumerate all the edges 
        // enumerate arcs that can be reached starting from the current node's children
        // [in/out] visited record already visited nodes 
        void EnumerateArcs(std::unordered_set<ComputationNodeBasePtr>& visited, std::list<ComputationArc>& arcs)
        {
            std::list<ComputationNodeBasePtr>	tovisit;

            if (visited.find(shared_from_this()) == visited.end()) // only do when this node has not been visited before
            {
                tovisit.push_back(shared_from_this());

                while (!tovisit.empty())
                {
                    ComputationNodeBasePtr curNode = tovisit.front();
                    tovisit.pop_front();

                    if (visited.find(curNode) == visited.end())
                    {
                        for (size_t i = 0; i < curNode->m_children.size(); i++)
                        {
                            arcs.push_back(ComputationArc(curNode, curNode->m_children[i]));

                            if (visited.find(curNode->m_children[i]) == visited.end()) // this children has not been visited before 
                                tovisit.push_front(curNode->m_children[i]);		// going to visit each of the children
                        }
                        visited.insert(curNode);
                    }
                }
            }
        }

        std::wstring CreateUniqNodeName() const
        {
#ifdef USE_GUID_AS_NAME
            UUID uuid;
            ZeroMemory(&uuid, sizeof(UUID));
            std::wstring name;

            UuidCreate(&uuid);
            WCHAR* szUuid = nullptr;
            if (UuidToStringW(&uuid, (RPC_WSTR*)&szUuid) != RPC_S_OK)
                RuntimeError("Failed to craete unique node name.");
            else
            {
                name = szUuid;
                RpcStringFreeW((RPC_WSTR*)&szUuid);
            }
#else
            int64_t id = CreateUniqId();
            std::wstring base = L"AutoName";
            std::wstringstream sstm;
            sstm << base.c_str() << id;
            std::wstring name = sstm.str();
            //msra::strfun::wstrprintf name(L"%s%d", L"AutoName", id);
#endif

            return name;
        }

        // TODO: These 4 functions will be completed after refactoring.
        //request matrices needed to do node function value evaluation
        virtual void RequestEvalMatrices(MatrixPool& matrixPool)
        {
            matrixPool;
        }

        //release temp matrices that are only used by forward computation
        //don't release matrices that need to be used in the gradient computation
        virtual void ReleaseMatricesAfterEval(MatrixPool& matrixPool)
        {
            matrixPool;
        }

        //request matrices that are needed for gradient computation
        virtual void RequestGradientMatrices(MatrixPool& matrixPool, const int numParents)
        {
            matrixPool; numParents;
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseGradientMatrices(MatrixPool& matrixPool)
        {
            matrixPool;
        }

    protected:
        // data members
        DEVICEID_TYPE m_deviceId; //CPU=-1, >=0 GPU
        std::wstring m_nodeName;

        MBLayoutPtr m_pMBLayout;

        std::vector<ComputationNodeBasePtr> m_children;

        bool m_parameterUpdateRequired;     // update parameters? Only used for LearnableParameters.    --TODO: Should we make this a member of LearnableParameters actually? And require a type cast? Currently it is read out for all leaves.

        ImageLayout m_inputImageLayout;     // how to interpret each column in the input as an image
        ImageLayout m_outputImageLayout;    // and the output
    };
    typedef ComputationNodeBase::ComputationNodeBasePtr ComputationNodeBasePtr;

    // =======================================================================
    // ComputationNode -- abstract base class for computation nodes parameterized by float vs. double
    // =======================================================================

    // little helper class to allow derived Node classes to specify how many inputs they expect
    struct INumInputs { virtual size_t GetExpectedNumInputs() const = 0; };
    template<size_t m_numInputs> struct NumInputs : public INumInputs { size_t GetExpectedNumInputs() const override { return m_numInputs; } };  // e.g. derive from NumInputs<2>

    template<class ElemType>
    class ComputationNode : public ComputationNodeBase // abstract class that cannot be instantiated
    {
        typedef ComputationNodeBase Base;
    protected:
        //std containers such as list and map does not support class reference so we need to use pointer
        typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;
        ComputationNode() { }
    public:
        using ComputationNodeBase::AttachInputs;    // import the convenience functions that take 1..6 parameters
        using ComputationNodeBase::Resize;
        typedef ElemType OurElemType;
    protected:
        // TODO: this should be protected and only accessible to the New method; maybe just move it in here?
        // TODO: Once we switch to VS 2015, we shall use inheriting constructors, i.e. we can delete all those redundant constructor forwards in each ComputationNode derivate
        // TODO: verify that we initialize all members (e.g. m_parameterUpdateRequired was missing before)
        ComputationNode(DEVICEID_TYPE deviceId, const wstring & name) :
            ComputationNodeBase(deviceId, name),
            m_functionValues(deviceId),
            m_gradientValues(deviceId)
        {
            InitRecurrentNode();
            ResetEvalTimeStamp();   // bring it into defined state
            // This constructor does not call MoveMatricesToDevice(), but that is needed for full initialization.
            // Only call this constructor through the New() factory below, which will ensure this.
        }
    public:
        // public constructor
        // You must construct ComputationNode derivates with this function. The real C++ constructor itself is hidden,
        // as we need to call a virtual function after construction. This function does that.
        template<class C, class... _Types> static inline shared_ptr<C> New(DEVICEID_TYPE deviceId, const wstring & name, _Types&&... _Args)
        {
            auto p = make_shared<C>(deviceId, name, forward<_Types>(_Args)...);     // creates objects, esp. assigns deviceId to matrices, but otherwise does nothing
            p->MoveMatricesToDevice(deviceId);                                      // this is a virtual call, i.e. it will handle extra matrices an object might own
            return p;
        }

        virtual ~ComputationNode()
        {
#ifdef DISPLAY_DEBUG
            fprintf (stderr, "Called Destructor NodeName: %s\n", (msra::strfun::utf8 (NodeName())).c_str()), fflush(stderr);
#endif
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId);

        // our own output dimensions
        /*implement*/size_t GetNumRows() const { return FunctionValues().GetNumRows(); }
        /*implement*/size_t GetNumCols() const { return FunctionValues().GetNumCols(); }
        /*implement*/void Resize(size_t rows, size_t cols)
        {
            FunctionValues().Resize(rows, cols);
#if 0//def _DEBUG
            fprintf(stderr, "Resize: Destructive resize to (%d x %d) in %ls %ls operation.\n", (int)rows, (int)cols, NodeName().c_str(), OperationName().c_str());
#endif
        }
        /*implement*/double Get00Element() const { return FunctionValues().Get00Element(); }

        // recover a shared_ptr from ourselves if given a naked pointer
        ComputationNodePtr shared_from_this()
        {
            return dynamic_pointer_cast<ComputationNode<ElemType>>(ComputationNodeBase::shared_from_this());
        }

        // recover a ComputationNodePtr (which is a shared_ptr) from a naked pointer to our base type (ComputationNodeBase) stored as a void* (old NDL parser does that)
        static ComputationNodePtr FromVoidPtr(void * vp)
        {
            auto p = dynamic_cast<ComputationNode<ElemType>*>((ComputationNodeBase*)vp);  // TODO: check that all void* casts really come from ComputationNodeBasePtr; or add a method ToVoidPtr(). Or get rid of the void*?!
            return p->shared_from_this();
        }

        // AttachInputs() -- attach the inputs of a node
        // This verifies the number of inputs. For that, nodes with fixed number of inputs derive from NumInputs<N>.
        // This function discovers this through RTTI and performs a runtime check. Nodes should not have additional checks in their implementation (save the code).
        // Note: Nodes with variable number of inputs will not derive from NumInputs<>, but instead check their inputs in Validate().
        void AttachInputs(const std::vector<ComputationNodeBasePtr>& inputs)
        {
            wstring name = NodeName(); name;
            const auto * pNumInputs = dynamic_cast<INumInputs*>(this);    // if this class also derives from NumInputs<N> then N is the expected number of inputs
            if (pNumInputs && pNumInputs->GetExpectedNumInputs() != inputs.size())
                RuntimeError("%ls operation '%ls' expects %d inputs (given: %d)", OperationName().c_str(), NodeName().c_str(), (int)pNumInputs->GetExpectedNumInputs(), (int)inputs.size());
            m_children.resize(inputs.size());
            for (size_t i = 0; i < m_children.size(); i++)
                if (inputs[i])
                    m_children[i] = UpCast(inputs[i]);          // (UpCast() checks the type; the assignment then downcasts it again)
                else
                    m_children[i] = nullptr;                    // during network creation, nullpts are possible
        }

        virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const;

        // TODO: similar to DumpInfo; used by ExperimentalNetworkBuilder test implementation
        /*HasToString::*/ wstring ToString() const
        {
            // we format it like "name : type rows x cols ( args )"
            wstring result = /*TidyName*/(NodeName()) + L" : " + OperationName();
            result.append(msra::strfun::wstrprintf(L" %d x %d", (int)m_functionValues.GetNumRows(), (int)m_functionValues.GetNumCols()));
            if (m_children.empty()) result.append(L" ()");
            else
            {
                wstring args;
                bool first = true;
                for (auto & child : m_children)
                {
                    if (first)
                        first = false;
                    else
                        args.append(L"\n");
                    args.append(/*TidyName*/(child->NodeName()));
                }
                result += L" " + NestString(args, L'(', true, ')');
            }
            return result;
        }

        // update size (#columns) of m_{function,gradient}Values to match MBLayout
        // This must be called right before EvaluateThisNode() the first time for a given minibatch.
        // The 'numCols' parameter is legacy and will go away.
        // Currently overridden by
        //  - InputValue, which verifies instead of resizing (since Resize() is specified to be destructive, it should not call it).
        //  - LearnableParameters
        //  - GMMLogLikelihoodNode (which allocates some internal temp memory).
        // Important: Unless overridden, this function is destructive. Nodes cannot carry over minibatch-size dependent state across minibatches through m_functionValues because of this.
        virtual size_t UpdateFunctionAndGradientMBSize(size_t numCols)
        {
            if (!m_pMBLayout)               // if no layout, this node contains parameters independent of MB size, don't resize
                return numCols;             // BUGBUG: what to return here?
            if (numCols == SIZE_MAX)        // SIZE_MAX means determine from layout
                numCols = m_pMBLayout->GetNumCols();
            m_functionValues.ResizeColumns(numCols);
            // BUGBUG: I have encountered a case where a MinusNode had m_parameterUpdateRequired false but a child had it set to true (Amit's Recipes2\3_seqRetrain_small)
            //         We will change now m_parameterUpdateRequired is propagated (separate into a node request and a cached flag about subtrees). Then reenable this code.
            if (m_needsGradient)    // TODO: This knowledge should be owned by Network
                m_gradientValues.ResizeColumns(numCols);
            else
                m_gradientValues.Resize(0,0);
            return numCols;
        }

        void ValidateInferChildDims(size_t i, size_t rows, size_t cols) override final;

#if 0   // (this function cannot be used currently since sentenceBegin is not a Matrix<ElemType> anymore; only affects LSTMNode which is no longer used)
        static void WINAPI SetToInitStateValueForResetSeg(const Matrix<ElemType>& sentenceBegin,
                                                          size_t nStream, ElemType initStateValue, Matrix<ElemType>& newprevstate)
        {
            Matrix<ElemType> colSeg(sentenceBegin.GetDeviceId());
            colSeg.Resize(nStream, nStream);
            size_t nStateRow = newprevstate.GetNumRows();

            assert(nStream == sentenceBegin.GetNumRows());

            /// only set state to init state value for segmentation = 0, and -1
            /// e.g., -1 0 1 -> 0 0 1 -> 0 0 -1 -> 1 1 0 

            Matrix<ElemType> colPos(sentenceBegin.GetDeviceId());
            colPos.SetValue(sentenceBegin); /// -1 0 1
            colPos.InplaceTruncateBottom(((int) MinibatchPackingFlags::SequenceStart));
            Matrix<ElemType>::Scale((ElemType)-1.0, colPos);
            colPos += ((int) MinibatchPackingFlags::None);
            // BUGBUG: ^^ What is this? colPos is a matrix, None is a flag; and it is 0
            colSeg.SetDiagonalValue(colPos);
            Matrix<ElemType> ones(sentenceBegin.GetDeviceId());
            ones.Resize(nStateRow, nStream);
            ones.SetValue((ElemType)1);
            /// add default state value if it is for reset
            Matrix<ElemType>::MultiplyAndWeightedAdd(initStateValue, ones, false, colSeg, false, 1.0, newprevstate);  /// += [0 initStateValue 0 ]
        }
#endif

        /**
        reset to error signals to 0 for any elements without labels
        */
        // This sets MB columns to 0 that have the NoLabel or NoFeature flag set.
        // This happens as a result of packing multiple sequences for parallel processing--there will be some gaps, which are flagged by these flags.
        // Nodes that operate in 'map' style (input(j) -> output(j) independently) can ignore this; it will be garbage-in-garbage-out.
        // However, nodes that 'reduce' minibatches (e.g. computing the sum of all frames across all sequences) must deal with the garbage.
        // This function sets those to 0, assuming that now they can be reduced without affecting the result.
        // This function can operate on the whole range or on a selected single frame and/or a single sequence.
        // It is indirectly guarded by the m_maskMissingColumnsToZero flag, which, if false, will install a layout with IsAllNone() to be true. TODO: we better always install the same layout, and instead test m_maskMissingColumnsToZero here.
        // Note that existing 'reduce' style operations--the criterion nodes and gradient computation--already call this.  --BUGBUG: They can't, wrong layout!
        // Warning: The layout used here must match the matrix. E.g. don't pass a child's matrix from a criterion node (use Inputs(x)->MaskMissing{Values,Gradient}ColumnsToZero() instead.
    private:
        static bool MaskMissingColumnsTo(Matrix<ElemType>& matrixToBeMasked, const MBLayoutPtr & pMBLayout, size_t timeIdxInSeq, size_t seqIndex, ElemType val)
        {
            bool foundLabelOrFeatureMissing = false;    // return value: set to true if either nolabel or feature missing is processed

            if (pMBLayout && !pMBLayout->IsAllNone())
            {
                size_t nT = pMBLayout->GetNumTimeSteps();
                size_t nS = pMBLayout->GetNumParallelSequences();

                if (matrixToBeMasked.GetNumCols() != nT * nS)
                    LogicError("MaskMissingColumnsToZero: pMBLayout->m_minibatchPackingFlags should have one element for each timestep of all streams. Check feature reader. ");

                size_t startT = (timeIdxInSeq == SIZE_MAX) ?  0 : timeIdxInSeq;
                size_t endT   = (timeIdxInSeq == SIZE_MAX) ? nT : timeIdxInSeq + 1;

                size_t startS = (seqIndex == SIZE_MAX) ?  0 : seqIndex;
                size_t endS   = (seqIndex == SIZE_MAX) ? nS : seqIndex + 1;

                for (size_t t = startT; t < endT; t++)
                {
                    FrameRange frameRange(t);
                    if (pMBLayout->Is(t, MinibatchPackingFlags::NoInput))
                    {
                        for (size_t s = startS; s < endS; s++)
                            if (pMBLayout->Is(s, t, MinibatchPackingFlags::NoInput))
                                //matrixToBeMasked.ColumnSlice(t * nS  +  s, 1).SetValue(val);
                                DataSlice(matrixToBeMasked, frameRange.Sequence(s), pMBLayout).SetValue(val);
                        foundLabelOrFeatureMissing = true;
                    }
                }
            }

            return foundLabelOrFeatureMissing;
        }
    public:
        static bool MaskMissingColumnsToZero(Matrix<ElemType>& matrixToBeMasked, const MBLayoutPtr & pMBLayout, size_t timeIdxInSeq = SIZE_MAX, size_t seqIndex = SIZE_MAX)
        {
            return MaskMissingColumnsTo(matrixToBeMasked, pMBLayout, timeIdxInSeq, seqIndex, 0);
        }

        // call static MaskMissingColumnsToZero() above with m_{function,gradient}Values with matching layout
        bool MaskMissingColumnsToZero(Matrix<ElemType>& matrixToBeMasked, size_t timeIdxInSeq = SIZE_MAX, size_t seqIndex = SIZE_MAX) const
        {
            return MaskMissingColumnsToZero(matrixToBeMasked, m_pMBLayout, timeIdxInSeq, seqIndex);
        }

        /*implement*/void MaskMissingValuesColumnsToZero()
        {
            MaskMissingColumnsToZero(m_functionValues);
        }
        /*implement*/void MaskMissingValuesColumnsToZero(const size_t timeIdxInSeq) // TODO: change to FrameRange as well
        {
            MaskMissingColumnsToZero(m_functionValues, timeIdxInSeq);
        }
        /*implement*/void MaskMissingGradientColumnsToZero()
        {
            MaskMissingColumnsToZero(m_gradientValues);
        }
        // TODO: use a FrameRange here as well, then unify with above
        /*implement*/void MaskMissingGradientColumnsToZero(const size_t timeIdxInSeq)
        {
            MaskMissingColumnsToZero(m_gradientValues, timeIdxInSeq);
        }

        // for debugging, set the gaps to NaN instead (to track whether it bubbles up somewhere)
        /*implement*/void MaskMissingValuesColumnsToNan()
        {
            MaskMissingColumnsTo(m_functionValues, m_pMBLayout, SIZE_MAX, SIZE_MAX, Matrix<ElemType>::MakeNan(__LINE__));
        }

        /*
        virtual size_t GetNumSamplesWithLabel(const size_t numAllSamples)
        {
            if (m_mbLayout.m_sentenceBoundaryFlags != nullptr &&
                m_mbLayout.m_minibatchPackingFlags != nullptr &&
                !m_mbLayout.m_sentenceBoundaryFlags->IsEmpty() &&
                !m_mbLayout.m_minibatchPackingFlags->size() == 0)
            {
                size_t numTimeSteps = m_mbLayout.m_sentenceBoundaryFlags->GetNumCols();
                size_t numSequences = m_mbLayout.m_sentenceBoundaryFlags->GetNumRows();

                if (m_mbLayout.m_minibatchPackingFlags->size() != numTimeSteps)
                {
                    LogicError("GetNumSamplesWithLabel(): m_mbLayout.m_minibatchPackingFlags should have one element for each timestep of all streams.Check feature reader. ");
                }

                size_t numSamplesWithoutLabel = 0;

                for (size_t j = 0; j < numTimeSteps; j++)
                {
                    if (m_pMBLayout->m_minibatchPackingFlags[j] & MinibatchPackingFlags::NoLabel)
                    {
                        for (int i = 0; i < numSequences; i++)
                        {
                            if ((int)m_pMBLayout->m_sentenceBoundaryFlags(i, j) & ((int) MinibatchPackingFlags::NoLabel))
                            {
                                numSamplesWithoutLabel++;
                            }
                        }
                    }
                }

                return numTimeSteps*numSequences - numSamplesWithoutLabel;
            }
            else
            {
                return numAllSamples;
            }
        }
        */

        // for debugging purpose
        void /*ComputationNodeBase::*/PrintSelf(bool printMatrices = false) const
        {
            fprintf(stderr, "\n%ls[%lu, %lu] = %ls", NodeName().c_str(), GetNumRows(), GetNumCols(), OperationName().c_str());           

            if (!IsLeaf())
            {
                fprintf(stderr, "(");           
                for (size_t i=0; i<ChildrenSize(); i++)
                {
                    if (i > 0)
                        fprintf(stderr, ", ");           
                    fprintf(stderr, "%ls[%lu, %lu]", m_children[i] ? m_children[i]->NodeName().c_str():L"NULL", m_children[i]->GetNumRows(), m_children[i]->GetNumCols());
                }
                fprintf(stderr, ")");           
            }

            if (printMatrices)
            {
                fprintf (stderr, "\n    $$$$ Function Values\n");
                FunctionValues().Print("FunctionValue");

                fprintf (stderr, "\n    $$$$ Gradient Values\n");
                GradientValues().Print("GradientValue");
            }
        }

        // up-cast to make life easier
        static ComputationNodePtr UpCast(ComputationNodeBasePtr inode)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(inode);
            if (!node)
                InvalidArgument("an ComputationNodeBasePtr of mismatching precision was passed");
            return node;
        }

        inline ComputationNodePtr Inputs(const size_t childIndex) const       // TODO: rename to Input
        {
#ifdef _DEBUG // profile shows this is range check very expensive in release mode, skip it  
            if (childIndex >= m_children.size())
                LogicError("Inputs: childIndex is out of range.");
#endif
            return UpCast(m_children[childIndex]);
        }

        /*implement*/void SetInput(const size_t childIndex, const ComputationNodeBasePtr inode)
        {
            const ComputationNodePtr node = UpCast(inode);

            //require first nodes specified before the second to avoid null nodes condition.
            if (childIndex > m_children.size())
                InvalidArgument("SetInput: You must specify the input for children with index less than this one first.");

            // expand the inputs to exist up to the desired index
            while (childIndex >= m_children.size())
                m_children.push_back(nullptr);

            // set the input value
            m_children[childIndex] = node;
        }

        // these are overridden by DropoutNode, ReshapeNode, and RowRepeatNode to optimize for the trivial case that those don't do anything
        // TODO: lots of nodes read out m_functionValues directly--was that a bug or intentional? They have now been changed to ValueSlice(), i.e. would pick it up
        virtual const Matrix<ElemType>& FunctionValues() const { return m_functionValues; }
        virtual Matrix<ElemType>& FunctionValues() { return m_functionValues; }

        const Matrix<ElemType>& GradientValues() const { return m_gradientValues; }
        Matrix<ElemType>& GradientValues() { return m_gradientValues; }

        // function to access any input and output, value and gradient, whole batch or single frame
        // Note: This returns an object, not a reference. That object is a column slice, i.e. a small object that just points into another object.
        // TODO: remove FrameRange::samplesInRecurrentStep from FrameRange, as it belongs into pMBLayout. Hence this function that binds both together.
        // Note: This is not used anywhere yet, only a sketch how we may further abstract timing.
        Matrix<ElemType> DataSlice(Matrix<ElemType> & data,
                                   const FrameRange & frameRange/*select frame or entire batch*/)
        {
            return DataSlice(data, frameRange, m_pMBLayout);
        }
        static Matrix<ElemType> DataSlice(Matrix<ElemType> & data,
                                          const FrameRange & frameRange/*select frame or entire batch*/,
                                          const MBLayoutPtr & pMBLayout)
        {
            // if FrameRange refers to whole minibatch (map mode)
            // or if we don't even have a layout
            // then return the whole matrix
            if (!pMBLayout || frameRange.IsAllFrames())
            {
                if (frameRange.seqIndex == SIZE_MAX)
                    return data.ColumnSlice(0, data.GetNumCols());
                else
                {
                    if (!pMBLayout)
                        LogicError("DataSlice: Attempting to retrieve a parallel sequence from data without layout.");
#if 1
                    else
                        LogicError("DataSlice: To retrieve a parallel sequence, implement Matrix::RowSlice() first!");
#else
                    // get a reshaped view that stacks all sequences into T long vectors
                    auto mat = data.ColumnSlice(0, data.GetNumCols());
                    mat.Resize(data.GetNumRows() * pMBLayout->GetNumParallelSequences(), data.GetNumRows() / pMBLayout->GetNumParallelSequences());
                    return mat;   // .RowSlice(frameRange.seqIndex * data.GetNumRows());
                    // TODO: Why does RowSlice() not exist? Seems simple. Is there a hidden assumption of contiguous memory?#endif
#endif
                }
            }
            // FrameRange refers to a time slice -> return that
            else
            {
                size_t numParallelSequences = pMBLayout->GetNumParallelSequences();
                size_t startColumn = frameRange.t() * numParallelSequences;
                if (frameRange.seqIndex == SIZE_MAX)
                    return data.ColumnSlice(startColumn, numParallelSequences);
                else
                    return data.ColumnSlice(startColumn + frameRange.seqIndex, 1);
            }
        }
        Matrix<ElemType> ValueSlice(const FrameRange & frameRange/*select frame or entire batch*/)
        {
            return DataSlice(FunctionValues(), frameRange);
        }
        Matrix<ElemType> GradientSlice(const FrameRange & frameRange/*select frame or entire batch*/)
        {
            return DataSlice(GradientValues(), frameRange);
        }

#ifdef _DEBUG
        virtual void /*IComputationNode::*/OnEvaluateEndIteration()               // called after last iteration step of EvaluateThisNode()
        {
            Base::OnEvaluateEndIteration();
#if 0       // NaN check
            MaskMissingValuesColumnsToZero();       // HasNaN() operates on a whole matrix, so first flatten all gaps to 0
            if (m_functionValues.HasNan("OnEvaluateEndIteration"))
                LogicError("%ls %ls operation unexpectedly produced NaN values.", NodeName().c_str(), OperationName().c_str());
#endif
            MaskMissingValuesColumnsToNan();        // blast NaNs into columns that are gaps in a packed layout
        }
#endif

        // this is the entry point from Network; while it will call virtual ComputeInputPartial() into the actual node implementation
        // TODO: This logic belongs into Network
        /*implement*/void ComputeGradientForChildren() override
        {
            if (HasLoop())
                return;

            for (size_t i = 0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = Inputs(i);
                if (child->m_needsGradient)
                {
                    //fprintf(stderr, "ComputeGradientForChildren: %ls %ls operation -> child %d %ls %ls\n", NodeName().c_str(), OperationName().c_str(), (int)i, child->NodeName().c_str(), child->OperationName().c_str());
                    if (!m_needsGradient)
                        LogicError("%ls %ls operation has m_needsGradient set to false but children require it.");
#ifdef DISPLAY_DEBUG
                    fprintf (stderr, "    [%lu]: %s(%s)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif              
#if DUMPOUTPUT
                    fprintf(stderr,"Backprop%d_%ls\n",i,NodeName().c_str());
#endif

                    ComputeInputPartial(i); //this computes partial wrt to the child and sums the gradient value in the child
#ifdef _DEBUG
#if 0               // NaN check
                    child->MaskMissingGradientColumnsToZero();  // hide NaNs in gaps (those are OK)
                    if (child->GradientValues().HasNan("ComputeGradientForChildren(void): "))
                        LogicError("%ls %ls operation has NaNs in gradient.", child->NodeName().c_str(), child->OperationName().c_str());
#endif
                    MaskMissingColumnsTo(child->GradientValues(), child->m_pMBLayout, SIZE_MAX, SIZE_MAX, Matrix<ElemType>::MakeNan(__LINE__));
#endif
                }
#ifdef DISPLAY_DEBUG
                else fprintf (stderr, "    [%lu]: %s(%s) (no gradient needed so don't compute for)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif
            }
        }

        // TODO: use a FrameRange here as well, then unify with above
        /*implement*/void ComputeGradientForChildren(const size_t timeIdxInSeq) override
        {
            for (size_t i = 0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = Inputs(i);
                if (child->m_needsGradient)
                {
#ifdef DISPLAY_DEBUG
                    fprintf (stderr, "    [%lu]: %s(%s)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif
                    //fprintf(stderr, "ComputeGradientForChildren: %ls %ls operation -> child %d %ls %ls\n", NodeName().c_str(), OperationName().c_str(), (int)i, child->NodeName().c_str(), child->OperationName().c_str());
                    ComputeInputPartial(i, FrameRange(timeIdxInSeq)); //this computes partial wrt to the child and sums the gradient value in the child
                }
#ifdef DISPLAY_DEBUG
                else fprintf (stderr, "    [%lu]: %s(%s) (no gradient needed so don't compute for)\n", i, 
                        (msra::strfun::utf8 (child->OperationName())).c_str(),
                        (msra::strfun::utf8 (child->NodeName())).c_str());
#endif
            }
        }

        /*implement*/void ClearGradientForChildren(const int /*iActMiniBatchSize*/)
        {
            for (size_t i=0; i<m_children.size(); i++)
            {
                ComputationNodePtr child = Inputs(i);
                if (child->m_needsGradient)
                {
                    if(child->GradientValues().GetMatrixType() == DENSE) 
                    {
                        child->GradientValues().Resize(child->GetNumRows(), child->GetNumCols());
                        child->GradientValues().SetValue(0); 
                    }
                    else
                    {
                        child->GradientValues().Reset();
                    }
                }
            }
        }

        // NOTE: we should reimplement this to be thread-safe and use a larger than requested initialized memory block
        // we can then just wrap that memory block in a matrix of the correct dimensions since it will be const no one can change it
        // should only need one memory block per device
        static const Matrix<ElemType>& ConstOnes(const size_t rows, const size_t cols, const DEVICEID_TYPE deviceId)
        {
            if (s_constOnes.find(rows) == s_constOnes.end() ||
                s_constOnes[rows].find(cols) == s_constOnes[rows].end()) //not found
            {
                Matrix<ElemType>* matrix = new Matrix<ElemType>(rows, cols, (DEVICEID_TYPE)deviceId);
                matrix->SetValue(1);
                s_constOnes[rows][cols] = matrix;
            }

            Matrix<ElemType>* m = s_constOnes[rows][cols];
            m->TransferFromDeviceToDevice(m->GetDeviceId(), deviceId);

            return *m;
        }

    protected:

        //to be called by derived classed if that class needs to print node values
        void PrintNodeValuesToFile(const bool printValues, File& fstream) const
        {
            if (printValues)
            {
                fstream << wstring(L"\n");
                const Matrix<ElemType>&  m = FunctionValues();
                for (size_t i=0; i < m.GetNumRows(); i++)
                {
                    for (size_t j=0; j < m.GetNumCols(); j++)
                    {
                        fstream << m(i,j);
                    }
                    fstream << wstring(L"\n");
                }
                fstream << wstring(L"####################################################################");
            }
        }

    public:
        /*implement*/void CopyTo(const ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            CopyTo(UpCast(node), newName, flags);
        }
        virtual void CopyTo(const ComputationNodePtr node, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            ComputationNodeBase::CopyTo(node, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_functionValues = m_functionValues; 
                node->m_gradientValues = m_gradientValues;
            }
        }

        // duplicate a node
        ComputationNodeBasePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags)
        {
            const std::wstring& name = (newName == L"") ? NodeName() : newName;
            ComputationNodeBasePtr node(NewThis(m_deviceId, name)); // NewThis() is a virtual function that creates a new node of the actual type of 'this'
            node->CopyTo(shared_from_this(), newName, flags);       // note: shared_from_this() is the base class, but CopyTo() up-casts it as needed
            return node;
        }

        // these are used to export hidden state activations
        virtual bool GetHistory(Matrix<ElemType>&, bool) { return false; }
        virtual void SetHistory(const Matrix<ElemType>&) { }

        /// these two are used to pass gradients from future minibatch
        virtual void GetErrorsToPreviousMinibatch(Matrix<ElemType>&) {}
        virtual void SetErrorsFromFutureMinibatch(Matrix<ElemType>&) {}

    protected:

        Matrix<ElemType> m_functionValues, m_gradientValues;

        static std::map<size_t, std::map<size_t, Matrix<ElemType>*>> s_constOnes;
    };

    // convenience wrapper for ComputationNode::New()
    template<class C, class... _Types> inline shared_ptr<C> New(DEVICEID_TYPE deviceId, const wstring & name, _Types&&... _Args)
    {
        return ComputationNode<typename C::OurElemType>::template New<C>(deviceId, name, forward<_Types>(_Args)...);
    }

    // =======================================================================
    // ComputationNodeNonLooping -- abstract base class for computation nodes that do not implement eval/partial for individual frames
    // Such as CRFNode, LSTMNode, ParallelNode, SequenceDecoderNode, TimeReverseNode (BatchModeNode), and TransposeNode.
    // =======================================================================

    // This will provide default implementations for those two functions that will fail at runtime with a meaningful error.
    // TODO: Most of these are reduce nodes that output a single number, no MBLayout. Maybe abstract those out further
    template<class ElemType>
    class ComputationNodeNonLooping : public ComputationNode<ElemType>
    {
        typedef ComputationNode<ElemType> Base;
    public:
        //virtual ComputationNodeBase * NewThis(DEVICEID_TYPE deviceId, const wstring & name) = 0;
        ComputationNodeNonLooping(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        // TODO: check range here? Or just make a helper function with the test? Or use DataSlice()??
        virtual void ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange &)
        {
            LogicError("%s node should never be in a loop.", typeid(*this).name());
        }
        // non-looping node types instead implement this function...
        virtual void EvaluateThisNodeNonLooping() = 0;
        // ...which we call from our overload, but not before we checked that indeed the entire batch is passed
        virtual void EvaluateThisNode(const FrameRange & frameRange)
        {
            if (frameRange.IsAllFrames())
                EvaluateThisNodeNonLooping();
            else
                LogicError("%s node should never be in a loop.", typeid(*this).name());
        }
        // classes that derive from this must implement the non-range version
        virtual void ComputeInputPartial(const size_t inputIndex) = 0;
    };

    // helper macro to ease access to base members in presence of C++ two-phase name lookup
    // Add 'typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;' at the start of each derived class
    // (some derived classes define a similar macro; there please modify the typedef for Base accordingly.)
    // This macro imports, one by one, every member of ComputationNode into the name space of the derived class.
    // Without this, one would have to use the name prefix, or alternatively this->, in front of all base member,
    // because the standard does not allow the compiler to do that for you (as MSVC still kindly does).
    // If you add new members to ComputationNode, please also add them here.
    // This macro expects 'Base' to be the name of the base class. Please also use 'Base' outside this macro to make it less likely to accidentally call the wrong base class members.
    // BUGBUG: some should be protected, not public
    // Note: Whoever invented that insanity called two-phase name lookup shall rot in hell, for the crime of causing infinite pain. [fseide]
#define UsingComputationNodeMembers /*-WithoutOperationName needed to support inconsistent pattern of InputValue */    \
protected: \
    typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;  /*TODO: can we just use 'using?' */ \
    using Base::Resize; using Base::GetNumRows; using Base::GetNumCols; \
    using Base::m_pMBLayout; using Base::GetNumTimeSteps; using Base::GetNumParallelSequences; \
    using Base::MaskMissingColumnsToZero; using Base::MaskMissingValuesColumnsToZero; using Base::MaskMissingGradientColumnsToZero; \
    using Base::DataSlice; using Base::ValueSlice; using Base::GradientSlice; \
    using Base::m_children; using Base::m_deviceId; using Base::m_functionValues; using Base::m_gradientValues; \
    using Base::m_inputImageLayout; using Base::m_outputImageLayout; \
    using Base::m_parameterUpdateRequired; using Base::m_nodeName; using Base::s_constOnes; \
    using Base::shared_from_this; \
public: \
    using Base::CreateUniqId; \
    using Base::AttachInputs; using Base::ChildrenNeedGradient; using Base::ChildrenSize; using Base::ClearGradientForChildren; using Base::VerifySize; \
    /*using Base::ComputeGradientForChildren; using Base::ComputeInputPartial;*/ using Base::ConstOnes; \
    using Base::InferImageDimsFromInput; using Base::InferImageDimsFromInputs; using Base::InferMBLayoutFromInputsForStandardCase; \
    using Base::CopyTo; using Base::CreateUniqNodeName; using Base::DetachInputs; \
    using Base::DumpNodeInfo; using Base::EnumerateNodes; \
    using Base::HasMBLayout; using Base::GetMBLayout; using Base::LinkToMBLayout; \
    using Base::EvaluateThisNode; using Base::FunctionValues; \
    using Base::GradientValues; using Base::HasLoop; using Base::InitRecurrentNode; using Base::Inputs; \
    using Base::IsChildAnImage; using Base::IsEqualTo; using Base::IsFuncValueOlderThanInputs; using Base::IsLeaf; using Base::IsSmaller; \
    using Base::LoadFromFile; using Base::MoveMatricesToDevice; using Base::NodeName; \
    using Base::PrintNodeValuesToFile; using Base::PrintSelfBeforeValidation; \
    using Base::RequiresPreCompute; using Base::ReshuffleNodes; using Base::ReshuffleNodesForEvalWithRecurrentLoops; \
    using Base::SaveToFile; using Base::UpdateFunctionAndGradientMBSize; using Base::SetInput; \
    using Base::Validate; using Base::ValidateUnaryMap; using Base::ValidateBinaryZip; using Base::ValidateUnaryReduce; using Base::ValidateBinaryReduce; using Base::ValidateInferBinaryChildrenDims; using Base::ValidateInferChildDims

#define ComputationNodeBoilerplate \
protected:    /* some boilerplate goes here */ \
    virtual const std::wstring OperationName() const override { return TypeName(); } \
    virtual ComputationNodeBase * NewThis(DEVICEID_TYPE deviceId, const wstring & name) override { return new typename std::remove_reference<decltype(*this)>::type(deviceId, name); }

#define UsingComputationNodeMembersBoilerplate \
    ComputationNodeBoilerplate; UsingComputationNodeMembers

#pragma endregion base computation class

}}}