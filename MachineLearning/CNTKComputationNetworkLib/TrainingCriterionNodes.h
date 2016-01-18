//
// <copyright file="TrainingCriterionNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include "ComputationNode.h"
#include "InputAndParamNodes.h"
#include "gammacalculation.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    // -----------------------------------------------------------------------
    /// SquareErrorNode (left, right)
    // -----------------------------------------------------------------------

    //note: to save computation the gradient may be scaled by an constant. 

    template<class ElemType>
    class SquareErrorNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SquareError"; }
    public:
        SquareErrorNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_leftMinusRight(deviceId)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 1)
                InvalidArgument("SquareError criteria only takes two inputs.");

            if (inputIndex == 0)  //left derivative
                ComputeInputPartialLeft(Inputs(0)->GradientValues(), GradientValues(), m_leftMinusRight);
            else
                ComputeInputPartialRight(Inputs(1)->GradientValues(), GradientValues(), m_leftMinusRight);
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& leftMinusRight)  
        {
            inputGradientValues.AddWithScaleOf(gradientValues.Get00Element(), leftMinusRight);
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& leftMinusRight)  
        {
            inputGradientValues.AddWithScaleOf(-gradientValues.Get00Element(), leftMinusRight);
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), m_leftMinusRight, shared_from_this());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1, Matrix<ElemType>& leftMinusRight, ComputationNodePtr curNode)  
        {
            leftMinusRight.AssignDifferenceOf(inputFunctionValues0, inputFunctionValues1);
            curNode->MaskMissingColumnsToZero(leftMinusRight);  //we are fine since it will only be called with full minibatch.
            ElemType v = leftMinusRight.FrobeniusNorm();
            functionValues.Resize(1,1);
            functionValues.SetValue(v*v/2);
#if NANCHECK
            functionValues.HasNan("SquareError");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            size_t index = 0;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            index = 1;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements())
                LogicError("SquareError operation: one of the operants has 0 element.");

            if (!(Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows()  &&  //match size
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols()) )
            {
                LogicError("The Matrix dimension in the SquareError operation does not match.");
            }       

            FunctionValues().Resize(1,1);
            m_leftMinusRight.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }       

        //virtual void AttachInputs(const ComputationNodePtr leftNode, const ComputationNodePtr rightNode) 
        //{
        //    m_children.resize(2);
        //    m_children[0] = leftNode;
        //    m_children[1] = rightNode;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_leftMinusRight.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<SquareErrorNode<ElemType>>(nodeP);
                node->m_leftMinusRight = m_leftMinusRight;
            }
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    private:
        Matrix<ElemType> m_leftMinusRight;
    };

    template class SquareErrorNode<float>; 
    template class SquareErrorNode<double>;

    // -----------------------------------------------------------------------
    // CrossEntropyWithSoftmaxNode (labels, prediction)
    // -----------------------------------------------------------------------

    //calculates: -sum(left_i * log(softmax_i(right)))
    template<class ElemType>
    class CrossEntropyWithSoftmaxNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CrossEntropyWithSoftmax"; }
    public:
        CrossEntropyWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmaxOfRight(deviceId), m_softmaxOfRight(deviceId)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            //left Node must be a scalar
            if (inputIndex == 0)  //left derivative
            {
                ComputeInputPartialLeft(m_logSoftmaxOfRight, Inputs(inputIndex)->GradientValues(), GradientValues());
            }
            else
            {
                ComputeInputPartialRight(m_softmaxOfRight, Inputs(0)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues());
                Base::MaskMissingColumnsToZero(Inputs(inputIndex)->GradientValues());
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(const Matrix<ElemType>& logSoftmaxOfRight, Matrix<ElemType>& inputGradientValues, 
            const Matrix<ElemType>& gradientValues)  
        {
#if DUMPOUTPUT
            logSoftmaxOfRight.Print("CrossEntropyWithSoftmax Partial-logSoftmaxOfRight");
            gradientValues.Print("CrossEntropyWithSoftmax Partial-gradientValues");
            inputGradientValues.Print("CrossEntropyWithSoftmaxNode Partial-Left-in");
#endif

            Matrix<ElemType>::ScaleAndAdd(-gradientValues.Get00Element(), logSoftmaxOfRight, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("CrossEntropyWithSoftmaxNode Partial-Left-out");
#endif

        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(const Matrix<ElemType>& softmaxOfRight, const Matrix<ElemType>& inputFunctionValues, 
            Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        {
#if DUMPOUTPUT
            softmaxOfRight.Print("CrossEntropyWithSoftmax Partial-softmaxOfRight");
            inputFunctionValues.Print("CrossEntropyWithSoftmax Partial-inputFunctionValues");
            gradientValues.Print("CrossEntropyWithSoftmax Partial-gradientValues");
            inputGradientValues.Print("CrossEntropyWithSoftmaxNode Partial-Right-in");
#endif

            Matrix<ElemType>::AddScaledDifference(gradientValues, softmaxOfRight, inputFunctionValues, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("CrossEntropyWithSoftmaxNode Partial-Right");
#endif
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override   //-sum(left_i * log(softmax_i(right)))
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), m_softmaxOfRight, m_logSoftmaxOfRight, shared_from_this());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1, 
            Matrix<ElemType>& softmaxOfRight, Matrix<ElemType>& logSoftmaxOfRight, ComputationNodePtr curNode)
        {
            logSoftmaxOfRight.AssignLogSoftmaxOf(inputFunctionValues1, true);
            softmaxOfRight.SetValue(logSoftmaxOfRight);
            softmaxOfRight.InplaceExp();
            curNode->MaskMissingColumnsToZero(logSoftmaxOfRight); //we are fine here since it will be called only with full minibatch
            functionValues.AssignInnerProductOfMatrices(inputFunctionValues0, logSoftmaxOfRight);
            functionValues*=(-1);
#if NANCHECK
            functionValues.HasNan("CrossEntropyWithSoftmax");
#endif
#if DUMPOUTPUT
            functionValues.Print("CrossEntropyWithSoftmaxNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            // This breaks re-shaping of the label matrix
            /*if (Inputs(0)->OperationName() != L"InputValue" && Inputs(0)->OperationName() != L"SparseInputValue")
            LogicError("CrossEntropyWithSoftmaxNode criterion requires the first input to be the label.");*/

            //we may release the constraint that the first operant is an inputValue later so the following code should be kept
            size_t index = 0;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            index = 1;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements())
                LogicError("CrossEntropyWithSoftmaxNode operation: one of the operants has 0 element.");

            if (!(Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows()  &&  //match size
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols()) )
            {
                LogicError("The Matrix<ElemType>  dimension in the CrossEntropyWithSoftmaxNode operation does not match.");
            }       

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 

            m_logSoftmaxOfRight.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_softmaxOfRight.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }

        //leftNode should be the empirical
        //virtual void AttachInputs(const ComputationNodePtr label, const ComputationNodePtr prediction) 
        //{
        //    m_children.resize(2);
        //    m_children[0] = label;
        //    m_children[1] = prediction;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_logSoftmaxOfRight.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_softmaxOfRight.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CrossEntropyWithSoftmaxNode<ElemType>>(nodeP);
                node->m_logSoftmaxOfRight = m_logSoftmaxOfRight;
                node->m_softmaxOfRight = m_softmaxOfRight;
            }
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    protected:
        Matrix<ElemType> m_logSoftmaxOfRight;
        Matrix<ElemType> m_softmaxOfRight;       
    };

    template class CrossEntropyWithSoftmaxNode<float>; 
    template class CrossEntropyWithSoftmaxNode<double>;

    // -----------------------------------------------------------------------
    /// CrossEntropyNode (labels, prediction)
    // -----------------------------------------------------------------------

    // calculates: -sum(left_i * log(right_i))
    // assume softmax is already done
    // You probably want to use CrossEntropyWithSoftMaxNode instead, it is more efficient in most cases.
    template<class ElemType>
    class CrossEntropyNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<2>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CrossEntropy"; }
    public:
        CrossEntropyNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logOfRight(deviceId), m_leftDivRight(deviceId)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            //left Node must be a scalar
            if (inputIndex == 0)  //left derivative
            {
                ComputeInputPartialLeft(m_logOfRight, Inputs(inputIndex)->GradientValues(), GradientValues());
            }
            else
            {
                ComputeInputPartialRight(m_leftDivRight, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues(), shared_from_this());
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(const Matrix<ElemType>& logOfRight, Matrix<ElemType>& inputGradientValues, 
            const Matrix<ElemType>& gradientValues)  
        {
            Matrix<ElemType>::ScaleAndAdd(-gradientValues.Get00Element(), logOfRight, inputGradientValues);
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& leftDivRight, 
            const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1,
            Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, ComputationNodePtr curNode)
        {
            leftDivRight.AssignElementDivisionOf(inputFunctionValues0, inputFunctionValues1);
            curNode->MaskMissingColumnsToZero(leftDivRight);
            Matrix<ElemType>::ScaleAndAdd(-gradientValues.Get00Element(), leftDivRight, inputGradientValues);
        }

        //-sum(left_i * log(right_i))
        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), m_logOfRight, shared_from_this());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1, 
            Matrix<ElemType>& logOfRight, ComputationNodePtr curNode)
        {
            logOfRight.SetValue(inputFunctionValues1);
            logOfRight.InplaceLog();
            curNode->MaskMissingColumnsToZero(logOfRight);
            functionValues.AssignInnerProductOfMatrices(inputFunctionValues0, logOfRight);
            functionValues*=(-1);
#if NANCHECK
            functionValues.HasNan("CrossEntropy");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->OperationName() != L"InputValue")
                LogicError("CrossEntropyNode criterion requires the first input to be the label.");

            //we may release the constraint that the first operant is an inputValue later so the following code should be kept
            size_t index = 0;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            index = 1;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0? Inputs(1-index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0? Inputs(1-index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements())
                LogicError("CrossEntropyNode operation: one of the operants has 0 element.");

            if (!(Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows()  &&  //match size
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols()) )
            {
                LogicError("The Matrix dimension in the CrossEntropyNode operation does not match.");
            }       

            FunctionValues().Resize(1,1);
            m_logOfRight.Resize(Inputs(1)->FunctionValues().GetNumRows(), Inputs(1)->FunctionValues().GetNumCols());
            m_leftDivRight.Resize(Inputs(1)->FunctionValues().GetNumRows(), Inputs(1)->FunctionValues().GetNumCols());
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }

        //leftNode should be the empirical
        //virtual void AttachInputs(const ComputationNodePtr label, const ComputationNodePtr prediction) 
        //{
        //    m_children.resize(2);
        //    m_children[0] = label;
        //    m_children[1] = prediction;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_logOfRight.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_leftDivRight.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CrossEntropyNode<ElemType>>(nodeP);
                node->m_logOfRight = m_logOfRight;
                node->m_leftDivRight = m_leftDivRight;
            }
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    private:
        // matrix value passed from evaluate to computePartial
        Matrix<ElemType> m_logOfRight;
        // temporary
        Matrix<ElemType> m_leftDivRight;
    };

    template class CrossEntropyNode<float>; 
    template class CrossEntropyNode<double>;

    // -----------------------------------------------------------------------
    // MatrixL1RegNode (input)
    // TODO: share most code with MatrixL2RegNode
    // -----------------------------------------------------------------------

    template<class ElemType>
    class MatrixL1RegNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<1>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"MatrixL1Reg"; }
    public:
        MatrixL1RegNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_gradientOfL1Norm(deviceId)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex) // scale by number of cols (or samples)
        {
            assert(inputIndex == 0); inputIndex;
            ComputeInputPartialS(m_gradientOfL1Norm, Inputs(0)->GradientValues(), GradientValues(), Inputs(0)->FunctionValues());
        }

        /*TODO: merge with call site*/void ComputeInputPartialS(Matrix<ElemType>& gradientOfL1Norm, 
            Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& inputFunctionValues)  
        {
            gradientOfL1Norm.AssignSignOf(inputFunctionValues);
            inputGradientValues.AddWithScaleOf(gradientValues.Get00Element(), gradientOfL1Norm);
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override  
        {
            Base::MaskMissingColumnsToZero(Inputs(0)->FunctionValues());
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues,  Matrix<ElemType>& inputFunctionValues)
        {
            functionValues.Resize(1, 1);
            functionValues.SetValue(inputFunctionValues.MatrixNorm1());
#if NANCHECK
            functionValues.HasNan("MatrixL1Reg");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->FunctionValues().HasNoElements())
                LogicError("MatrixL1Reg operation: the input node has 0 element.");

            FunctionValues().Resize(1,1);
            m_gradientOfL1Norm.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;
        }

        //virtual void AttachInputs(const ComputationNodePtr singleInput) 
        //{
        //    m_children.resize(1);
        //    m_children[0] = singleInput;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_gradientOfL1Norm.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<MatrixL1RegNode<ElemType>>(nodeP);
                node->m_gradientOfL1Norm = m_gradientOfL1Norm;
            }
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    private:
        Matrix<ElemType> m_gradientOfL1Norm;    // temporary
    };

    template class MatrixL1RegNode<float>; 
    template class MatrixL1RegNode<double>;

    // -----------------------------------------------------------------------
    // MatrixL2RegNode (input)
    // TODO: share most code with MatrixL1RegNode
    // -----------------------------------------------------------------------

    template<class ElemType>
    class MatrixL2RegNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<1>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"MatrixL2Reg"; }
    public:
        MatrixL2RegNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_temp(deviceId)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex) // scale by number of cols (or samples)
        {
            assert(inputIndex == 0); inputIndex;
            ComputeInputPartialS(Inputs(0)->GradientValues(), GradientValues(), Inputs(0)->FunctionValues(), FunctionValues());
        }

        /*TODO: merge with call site*/void ComputeInputPartialS(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& inputFunctionValues, const Matrix<ElemType>& functionValues)  
        {
            ElemType v = gradientValues.Get00Element() / (functionValues.Get00Element() + EPS_IN_INVERSE);
            inputGradientValues.AddWithScaleOf(v, inputFunctionValues);
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override  
        {
            Base::MaskMissingColumnsToZero(Inputs(0)->FunctionValues());
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues,  Matrix<ElemType>& inputFunctionValues)
        {
            functionValues.Resize(1,1);
            functionValues.SetValue(inputFunctionValues.FrobeniusNorm());
#if NANCHECK
            functionValues.HasNan("MatrixL2Reg");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->FunctionValues().HasNoElements())
                LogicError("MatrixL2Reg operation: the input node has 0 element.");

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }

        //virtual void AttachInputs(const ComputationNodePtr singleInput) 
        //{
        //    m_children.resize(1);
        //    m_children[0] = singleInput;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_temp.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    private:
        Matrix<ElemType> m_temp;
    };

    template class MatrixL2RegNode<float>; 
    template class MatrixL2RegNode<double>;

    // -----------------------------------------------------------------------
    /// NoiseContrastiveEstimationNode (labels, input, inputWeights, biasWeights)
    // -----------------------------------------------------------------------

    enum NCEEvalMode
    {
        Softmax = 0,
        Unnormalized = 1,
        None = 2
    };
    template<class ElemType>
    class NoiseContrastiveEstimationNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<4>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"NCEBasedCrossEntropyWithSoftmax"; }
    public:
        NoiseContrastiveEstimationNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmax(deviceId),
            m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_ncePrediction(deviceId),
            m_evalMode(NCEEvalMode::None)
        { }
        NoiseContrastiveEstimationNode(DEVICEID_TYPE deviceId, const wstring & name, NCEEvalMode xm_evalMode) :
            Base(deviceId, name),
            m_logSoftmax(deviceId),
            m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_ncePrediction(deviceId),
            m_evalMode(xm_evalMode)
        { }
        // ^^ TODO: we can merge these two

        virtual void SaveToFile(File& fstream) const override
        {
            Base::SaveToFile(fstream);
            fstream << m_evalMode;
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion) override
        {
            Base::LoadFromFile(fstream, modelVersion);
            fstream >> m_evalMode;
            if (m_evalMode > NCEEvalMode::None)
            {
                m_evalMode = NCEEvalMode::None;
                fstream.SetPosition(fstream.GetPosition() - sizeof(m_evalMode));
            }
        }

        void SetEvalMode(NCEEvalMode& xevMode) { m_evalMode = xevMode; }
        NCEEvalMode & EvalMode() { return m_evalMode; } // TODO: really? Return a reference to a local? TODO: change to const? and call it GetEvalMode()

        /**
        compute gradients to input observations, the weights to the observations, and the class log posterior probabilities
        */
        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            m_needRecomputeGradientToSoftmaxInput = false;
            //gradient computation@yinggongzhao
            //inputIndex should be 2 this time
            if (m_evalMode != NCEEvalMode::None)
                LogicError("ComputeInputPartial should only be called in training mode");
            if (inputIndex == 0)
                InvalidArgument("ComputeInput partial should not be called for label");
            //                                                                              samples+probs                   hidden                  embedding
            Inputs(inputIndex)->GradientValues().AssignNCEDerivative(m_ncePrediction, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), inputIndex);
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, true, inputGradientValues);
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(const Matrix<ElemType>& obs, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(obs, false, gradientValues, false, inputGradientValues);
        }

        static void WINAPI ComputeCEPartialToSoftmaxInputs(Matrix<ElemType>& inputGradientValues, Matrix<ElemType>& gradientValues, size_t y_t)
        {
            Matrix<ElemType>::MinusOneAt(inputGradientValues, y_t);
            Matrix<ElemType>::Scale(gradientValues, inputGradientValues);
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override   //-sum(left_i * log(softmax_i(right)))
        {
            int positive = 0, negative = 0;
            if (Inputs(0)->FunctionValues().GetNumRows() == 1)
            {
                for (int i = 0; i < Inputs(0)->FunctionValues().GetNumCols(); i++)
                {
                    if (Inputs(0)->FunctionValues()(0, i) > 0)
                        positive++;
                    else if (Inputs(0)->FunctionValues()(0, i) < 0)
                        negative++;
                }
                assert(positive * negative == 0);
            }
            if (m_evalMode == NCEEvalMode::Softmax || (Inputs(0)->FunctionValues().GetNumRows() == 1 && positive > 0))
            {
                // evaluation uses softmax
                m_logSoftmax.AssignProductOf(Inputs(1)->FunctionValues(), true, Inputs(2)->FunctionValues(), false);
                m_logSoftmax += Inputs(3)->FunctionValues();
                m_logSoftmax.InplaceLogSoftmax(false);
                FunctionValues().AssignSoftmaxSum(Inputs(0)->FunctionValues(), m_logSoftmax);
            }
            else if (m_evalMode == NCEEvalMode::Unnormalized || (Inputs(0)->FunctionValues().GetNumRows() == 1 && negative > 0))
            {
                FunctionValues().AssignNceUnnormalizedEval(Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues());
            }
            else
            {
                // training criterion uses NCE
                //likelihood                                         samples+probs                        hidden                       embedding            bias
                FunctionValues().AssignNoiseContrastiveEstimation(Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), m_ncePrediction);
            }
            m_needRecomputeGradientToSoftmaxInput = true;
        }

        /**
        Inputs: [0] label in dense matrix in [4 x T]
        the first row is the word index, the second row is the class index, the third row is the first word index of the class
        the last row is the first word index of the next class
        [1] hidden layer activity to the node in [hdsize x T]. for a simple rnn, this is the hidden layer activty
        [2] weight matrix in [hdsize x vocab_size], for speed-up, as per word matrix can be simply obtained as column slice
        [3] clsprob in dense matrix in [nbr_cls x T]. this is the output from logsoftmax node for the log-posterior probabilty of class given observations
        */
        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->OperationName() != OperationNameOf(InputValue))
                LogicError("NoiseContrastiveEstimationNode criterion requires the first input to be the label.");
            if (!(Inputs(1)->FunctionValues().GetNumRows() == Inputs(2)->FunctionValues().GetNumRows())) // input and matrix can be timed
                LogicError("The Matrix<ElemType>  dimension for observation and weight in the NoiseContrastiveEstimationNode operation does not match.");
            if (!(Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols())) // label and input same obs numbers
                LogicError("The Matrix<ElemType>  dimension for label and observation in the NoiseContrastiveEstimationNode operation does not match.");
            //if (!(Inputs(0)->FunctionValues().GetNumRows() == 3)) // label needs to be 4 rows
            //  LogicError("The label in the NoiseContrastiveEstimationNode operation needs to be 4 rows.");

            //cerr << Inputs(3)->FunctionValues().GetNumCols() << "\t" << Inputs(0)->FunctionValues().GetNumCols() << endl;
            //if (!(Inputs(3)->FunctionValues().GetNumCols() == Inputs(0)->FunctionValues().GetNumCols())) // number of observations
            //   LogicError("The number of observations in class log post probability and label in the NoiseContrastiveEstimationNode operation don't match.");
            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);
            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;
        }

        //virtual void AttachInputs(const ComputationNodePtr label, const ComputationNodePtr input,
        //    const ComputationNodePtr inputweight, const ComputationNodePtr biasWeight)
        //{
        //    m_children.resize(4);
        //    m_children[0] = label;
        //    m_children[1] = input;
        //    m_children[2] = inputweight;
        //    m_children[3] = biasWeight;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_logSoftmax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_softMax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_grdToSoftMaxInput.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    protected:
        Matrix<ElemType> m_logSoftmax;
        Matrix<ElemType> m_softMax;
        Matrix<ElemType> m_ncePrediction;

        /// gradient of cross entropy with respect to the input of softmax
        /// a 1 row by \sum_t m_nbrWordsInEachTime[t] vector
        /// one slice of size m_nbrWordsInEachTime[t] saves the input to softmax for word y_t
        Matrix<ElemType> m_grdToSoftMaxInput;
        bool m_needRecomputeGradientToSoftmaxInput;

        size_t m_nbrNoise;
        //size_t           m_nbrCls;//number class
        size_t           m_totalNbrWords;
    private:
        NCEEvalMode m_evalMode;
    };
    template class NoiseContrastiveEstimationNode<float>;
    template class NoiseContrastiveEstimationNode<double>;

    // -----------------------------------------------------------------------
    /// ClassBasedCrossEntropyWithSoftmaxNode (labels, input, inputweights, clsProbBeforeSoftmax)
    // -----------------------------------------------------------------------

    //calculates: -sum(left_i * log(softmax_i(right))) for class given history and for word given history
    // need to provide class probabilty from external node
    template<class ElemType>
    class ClassBasedCrossEntropyWithSoftmaxNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<4>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"ClassBasedCrossEntropyWithSoftmax"; }
    public:
        ClassBasedCrossEntropyWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmax(deviceId), m_softMax(deviceId), m_grdToSoftMaxInput(deviceId), m_clsLogSoftmax(deviceId), m_clsSoftmax(deviceId)
        { }

        /**
        compute gradients to input observations, the weights to the observations, and the class log posterior probabilites
        */
        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex != 1 && inputIndex != 2 && inputIndex != 3)
                InvalidArgument("ClassCrossEntropyWithSoftmaxNode criterion only takes with respect to input, weight to the input and class log posterior probability.");

            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            Matrix<ElemType> grd_t;
            Matrix<ElemType> grd_to_wgt_t;

            ComputeSoftMaxPartial();

            size_t sz = 0;
            for (size_t t = 0; t < nT; t++)
            {
                FrameRange frameRange(t, 1);    // TODO: change to frameRange over a whole MB with a sequence index. BUGBUG: below code will break until this is fixed
                /// compute prb - 1 and prb
                Matrix<ElemType> lbl_t = Inputs(0)->ValueSlice(frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));
                size_t c_t = (size_t)lbl_t(1, 0);
                size_t lft_bnd = (size_t)lbl_t(2, 0);
                size_t rgt_bnd = (size_t)lbl_t(3, 0);
                size_t nbr_wrd = rgt_bnd - lft_bnd; // number of words in the class
                if (nbr_wrd == 0)
                    continue;

                Matrix<ElemType> input_weight_t = Inputs(2)->FunctionValues().ColumnSlice(lft_bnd, nbr_wrd);
                Matrix<ElemType> obs = Inputs(1)->ValueSlice(frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));
                Matrix<ElemType> grd_to_soft_max_input = m_grdToSoftMaxInput.ColumnSlice(sz, nbr_wrd);
                Matrix<ElemType> grd_to_cls_prob = DataSlice(m_clsLogSoftmax, frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));

                switch (inputIndex){
                case 1:
                    /// gradient to input
                    grd_t = Inputs(1)->GradientSlice(frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));
                    ComputeInputPartialRight(input_weight_t, grd_t, grd_to_soft_max_input);
                    break;
                case 2:
                    /// gradient to input weight
                    grd_to_wgt_t = Inputs(2)->GradientValues().ColumnSlice(lft_bnd, nbr_wrd);
                    ComputeInputPartialLeft(obs, grd_to_wgt_t, grd_to_soft_max_input);
                    break;
                case 3:
                    grd_t = Inputs(3)->GradientSlice(frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));
                    grd_t.SetValue(DataSlice(m_clsSoftmax, frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout)));
                    ComputeCEPartialToSoftmaxInputs(grd_t, GradientValues(), c_t);
                    break;
                default:
                    RuntimeError("ClassCrossEntropyWithSoftmaxNode criterion only takes with respect to input, weight to the input and class log posterior probability.");
                }

                sz += nbr_wrd;
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, true, inputGradientValues);
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(const Matrix<ElemType>& obs, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType>::MultiplyAndAdd(obs, false, gradientValues, false, inputGradientValues);
        }

        static void WINAPI ComputeCEPartialToSoftmaxInputs(Matrix<ElemType>& inputGradientValues, Matrix<ElemType>& gradientValues, size_t y_t)
        {
            Matrix<ElemType>::MinusOneAt(inputGradientValues, y_t);
            Matrix<ElemType>::Scale(gradientValues, inputGradientValues);
        }

        /// gradient of cross entropy w.r.t. to input to softmax
        void ComputeSoftMaxPartial()
        {
            if (m_needRecomputeGradientToSoftmaxInput)
            {
                m_grdToSoftMaxInput.Resize(1, m_totalNbrWords);

                size_t nT = Inputs(1)->FunctionValues().GetNumCols();
                size_t sz = 0;
                for (size_t t = 0; t < nT; t++)
                {
                    FrameRange frameRange(t, 1);
                    /// compute prb - 1 and prb
                    Matrix<ElemType> lbl_t = Inputs(0)->ValueSlice(frameRange/*TODO: delete this:*/.Check(t, 1, m_pMBLayout));
                    size_t y_t = (size_t)lbl_t(0, 0);
                    size_t lft_bnd = (size_t)lbl_t(2, 0);
                    size_t rgt_bnd = (size_t)lbl_t(3, 0);
                    size_t nbr_wrd = rgt_bnd - lft_bnd;// number of words in the class

                    if (nbr_wrd == 0)
                    {
                        if (y_t == 0)
                            /// initialization of labels is usually zero, this case corresponds to no label is assigned at that time
                            continue; /// skip this time, because there is no label
                        else
                            LogicError("ClassbasedCrossEntropyWithSoftmax::ComputeSoftMaxPartial label provided but the size of its class is zero. Should never happen. Probably misuse of ClassbasedCrossEntropyWithSoftmax.");
                    }

                    Matrix<ElemType> softMax = m_softMax.ColumnSlice(sz, nbr_wrd);

                    ComputeCEPartialToSoftmaxInputs(softMax, GradientValues(), y_t - lft_bnd);

                    m_grdToSoftMaxInput.ColumnSlice(sz, nbr_wrd).SetValue(softMax);

                    sz += nbr_wrd;
                }

                m_needRecomputeGradientToSoftmaxInput = false;
            }
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override   //-sum(left_i * log(softmax_i(right)))
        {
            if (Inputs(0)->FunctionValues().GetDeviceId() != CPUDEVICE)
                LogicError("ClassBasedCrossEntropyWithSoftmax: evaluatethisnode. the label matrix is not using CPU device. This will make computation slow, even though the label data is probably saved on GPU. Because of the external loop over time with explicit class id retrieved from the label matrix, the computation will be very slow if the label matrix is saved on GPU. However, this is only a constraint for label matrix and other matrices such as data are suggested to reside on GPU. ");

            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(),
                              Inputs(3)->FunctionValues(), m_logSoftmax, m_softMax, m_clsLogSoftmax, m_clsSoftmax, m_totalNbrWords, this);
            m_needRecomputeGradientToSoftmaxInput = true;
        }

        static void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& lbls,
                                      const Matrix<ElemType>& inputs, const Matrix<ElemType>& input_weight, const Matrix<ElemType>& input_cls_log_post_prob,
                                      Matrix<ElemType>& logSoftmax,
                                      Matrix<ElemType>& softMax, 
                                      Matrix<ElemType>& clsLogSoftmax, Matrix<ElemType>& clsSoftmax, size_t& totalWords, ClassBasedCrossEntropyWithSoftmaxNode* curNode)
        {
            totalWords = 0;
            size_t nT = lbls.GetNumCols();

            for (size_t t = 0; t < lbls.GetNumCols(); t++)
            {
                Matrix<ElemType> lblInfo = lbls.ColumnSlice(t, 1);
                size_t lft_bnd = (size_t)lblInfo(2, 0);
                size_t rgt_bnd = (size_t)lblInfo(3, 0);
                totalWords += (rgt_bnd - lft_bnd);
            }

            size_t nRow = inputs.GetNumRows();

            size_t sz = totalWords;
            softMax.Resize(1, sz);
            logSoftmax.Resize(1, sz);
            clsLogSoftmax.Resize(input_cls_log_post_prob.GetNumRows(), nT);
            clsSoftmax.Resize(input_cls_log_post_prob.GetNumRows(), nT);

            clsLogSoftmax = input_cls_log_post_prob;
            clsLogSoftmax.InplaceLogSoftmax(true); /// 50 x nT
            clsSoftmax.AssignExpOf(clsLogSoftmax);

            /// loop over time
            functionValues.SetValue(0);
            sz = 0;
            for (size_t t = 0; t < lbls.GetNumCols(); t++)
            {
                Matrix<ElemType> lblInfo = lbls.ColumnSlice(t, 1);
                size_t y_t = (size_t)lblInfo(0, 0);
                size_t c_t = (size_t)lblInfo(1, 0);
                size_t lft_bnd = (size_t)lblInfo(2, 0);
                size_t rgt_bnd = (size_t)lblInfo(3, 0);
                size_t nbr_wrd = rgt_bnd - lft_bnd;

                if (nbr_wrd == 0)
                {
                    if (y_t == 0)
                        /// initialization of labels is usually zero, this case corresponds to no label is assigned at that time
                        /// skip this time
                        continue;
                    else
                        LogicError("ClassbasedCrossEntropyWithSoftmax::EvaluateThisNodeS label provided but the size of its class is zero. Should never happen. Probably misuse of ClassbasedCrossEntropyWithSoftmax.");
                }

                /// e.g., 200 x 148
                Matrix<ElemType> weightForClass = input_weight.ColumnSlice(lft_bnd, nbr_wrd);

                /// W x_t 
                Matrix<ElemType> softMax_t = softMax.ColumnSlice(sz, nbr_wrd);
                Matrix<ElemType> logSoftMax_t = logSoftmax.ColumnSlice(sz, nbr_wrd);

                if (!curNode->MaskMissingColumnsToZero(logSoftMax_t, t))
                {
                    Matrix<ElemType> obs = inputs.ColumnSlice(t, 1);  /// e.g., 200 x 1
                    obs.Reshape(1, nRow);  /// 1 x 200

                    logSoftMax_t.AssignProductOf(obs, false, weightForClass, false); /// 1 x 148

                    // log softmax(W x_t)
                    logSoftMax_t.InplaceLogSoftmax(false); /// 1 x 148
                    softMax_t.SetValue(logSoftMax_t);
                    // softmax(W x_t)
                    softMax_t.InplaceExp();  /// 1 x 148

                    /// add the word log posterior probability
                    if (y_t < lft_bnd)
                        LogicError("ClassBasedCrossEntropyWithSoftmax::EvaluateThisNodeS : the word index is smaller than its left bound of its class. This could happen because of reader issues. ");

                    size_t idx_in_class = y_t - lft_bnd;
                    Matrix<ElemType>::AddElementToElement(logSoftMax_t, 0, idx_in_class, functionValues, 0, 0);
                }

                /// add the class log posterior probability
                if (!curNode->MaskMissingColumnsToZero(clsLogSoftmax, t))
                {
                    try{
                        Matrix<ElemType>::AddElementToElement(clsLogSoftmax, c_t, t, functionValues, 0, 0);
                    }
                    catch (...)
                    {
                        fprintf(stderr, "EvaluateThisNodeS for ClassBasedCrossEntropyWithSoftmaxNode : number of classes is smaller than the dimension to read. Check network builder such as nbrClass and vocabulary file with class index to see if the number of classes and the maximum class index match. The right number should be number of classes == maximum class index number + 1\n");
                        throw;
                    }
                }

                sz += nbr_wrd;
            }

            functionValues *= (-1);

#if NANCHECK
            functionValues.HasNan("ClassBasedCrossEntropyWithSoftmax");
#endif
        }

        /**
        reset to error signals to 0 for any elements without labels
        */
        // BUGBUG: the layout should be that of matrixToBeMasked, not of 'this'
        bool MaskMissingColumnsToZero(Matrix<ElemType>& matrixToBeMasked, const size_t j) const
        {
            size_t nS = m_pMBLayout->GetNumParallelSequences();
            size_t t = j / nS;  // this is the time stamp
            size_t id = j % nS;  // this is the stream
            return Base::MaskMissingColumnsToZero(matrixToBeMasked, t, id);
#if 0       // old version prior to merging with Base version
            bool foundLabelOrFeatureMissing = false; /// set to true if either nolabel or feature missing is processed 

            if (!m_pMBLayout->IsAllNone())
            {
                if (m_pMBLayout->Is(t, MinibatchPackingFlags::NoLabel)) // TODO: this outer test is redundant here, no?
                {
                    if (m_pMBLayout->Is(id, t, MinibatchPackingFlags::NoLabel))
                    {
                        matrixToBeMasked.ColumnSlice(t * nS + id,1).SetValue(0);
                        foundLabelOrFeatureMissing = true;
                    }
                }
            }

            return foundLabelOrFeatureMissing;
#endif
        }

        /**
        Inputs: [0] label in dense matrix in [4 x T]
        the first row is the word index, the second row is the class index, the third row is the first word index of the class
        the last row is the first word index of the next class
        [1] hidden layer activity to the node in [hdsize x T]. for a simple rnn, this is the hidden layer activty
        [2] weight matrix in [hdsize x vocab_size], for speed-up, as per word matrix can be simply obtained as column slice
        [3] clsprob in dense matrix in [nbr_cls x T]. this input, if applied softmax on, is the posterior probabilty of class given observations
        */
        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->OperationName() != OperationNameOf(InputValue))
                LogicError("ClassBasedCrossEntropyWithSoftmaxNode criterion requires the first input to be the label.");
            if (!(Inputs(1)->FunctionValues().GetNumRows() == Inputs(2)->FunctionValues().GetNumRows())) // input and matrix can be timed
                LogicError("The Matrix<ElemType>  dimension for observation and weight in the ClassBasedCrossEntropyWithSoftmaxNode operation does not match.");
            if (!(Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols())) // label and input same obs numbers
                LogicError("The Matrix<ElemType>  dimension for label and observation in the ClassBasedCrossEntropyWithSoftmaxNode operation does not match.");
            if (!(Inputs(0)->FunctionValues().GetNumRows() == 4)) // label needs to be 4 rows
                LogicError("The label in the ClassBasedCrossEntropyWithSoftmaxNode operation needs to be 4 rows.");
            if (!(Inputs(3)->FunctionValues().GetNumCols() == Inputs(0)->FunctionValues().GetNumCols())) // number of observations
                LogicError("The number of observations in class log post probability and label in the ClassBasedCrossEntropyWithSoftmaxNode operation don't match.");

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs();

            m_nbrCls = Inputs(3)->FunctionValues().GetNumRows();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;
        }

        //virtual void AttachInputs(const ComputationNodePtr label, const ComputationNodePtr input,
        //    const ComputationNodePtr inputweight, const ComputationNodePtr clsProbBeforeSoftmax)
        //{
        //    m_children.resize(4);
        //    m_children[0] = label;
        //    m_children[1] = input;
        //    m_children[2] = inputweight;
        //    m_children[3] = clsProbBeforeSoftmax;
        //}

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_logSoftmax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_softMax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_clsLogSoftmax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_clsSoftmax.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_grdToSoftMaxInput.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    protected:
        Matrix<ElemType> m_logSoftmax;
        Matrix<ElemType> m_softMax;

        Matrix<ElemType> m_clsLogSoftmax;
        Matrix<ElemType> m_clsSoftmax;

        /// gradient of cross entropy with respect to the input of softmax
        /// a 1 row by \sum_t m_nbrWordsInEachTime[t] vector
        /// one slice of size m_nbrWordsInEachTime[t] saves the input to softmax for word y_t
        Matrix<ElemType> m_grdToSoftMaxInput;
        bool m_needRecomputeGradientToSoftmaxInput;

        size_t           m_nbrCls;
        size_t           m_totalNbrWords;
    };

    template class ClassBasedCrossEntropyWithSoftmaxNode<float>;
    template class ClassBasedCrossEntropyWithSoftmaxNode<double>;

    // -----------------------------------------------------------------------
    // CRFNode (labels, position_dependent_scores, transition_scores)
    //  - labels : output label vector of [0:T-1]
    //  - position_dependent_scores : score from position dependent node,
    //    in the R-CRF case, it is the RNN output score before softmax
    //  - transition scores : score from the transition node, 
    //    in the R-CRF case, it is the transition probability between labels
    // -----------------------------------------------------------------------

    /**
        CRF training criterion 
        It uses forward-backward algorithm within a minibatch to compute statistics for sequence level optimization 
        This node can serve a base class for other sequence level optimization

        Developed by Kaisheng Yao
        This node is for replicating results of the following work
        K. Yao, B. Peng, G. Zweig, D. Yu, X. Li and F. Gao, "Recurrent Conditional Random Fields", NIPS Deep Learning Workshop 2014
        K. Yao, B. Peng, G. Zweig, D. Yu, X. Li and F. Gao, "Recurrent Conditional Random Fields for Language Understanding", ICASSP 2014 
        http://research.microsoft.com/pubs/210167/rcrf_v9.pdf

        The forward-backward algorithm follows the derivation in 
        http://jmlr.org/papers/volume12/collobert11a/collobert11a.pdf

    */
    template<class ElemType>
    class CRFNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<3>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CRF"; }
    public:
        CRFNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            mAlpha(deviceId), mBeta(deviceId), mPostProb(deviceId)
        { }

        /// compute posterior probability of label y at position t
        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
            size_t nrow = Inputs(0)->FunctionValues().GetNumRows();
            size_t ncol = Inputs(0)->FunctionValues().GetNumCols();

            mAlpha.Resize(nrow, ncol);
            mBeta.Resize(nrow, ncol);
            mPostProb.Resize(nrow, ncol);

            FunctionValues().SetValue(0.0);
            Matrix<ElemType> funcVal = FunctionValues();

            size_t nstep = ncol / GetNumParallelSequences();
            for (size_t i = 0; i < GetNumParallelSequences(); i++)
            {
                Matrix<ElemType> postProbSlice = mPostProb.ColumnSlice(i * nstep, nstep);
                Matrix<ElemType> alphaSlice = mAlpha.ColumnSlice(i * nstep, nstep);
                Matrix<ElemType> betaSlice = mBeta.ColumnSlice(i * nstep, nstep);
                Matrix<ElemType> labelSlice = Inputs(0)->FunctionValues().ColumnSlice(i * nstep, nstep);
                Matrix<ElemType> posScoreSlice = Inputs(1)->FunctionValues().ColumnSlice(i * nstep, nstep);

                EvaluateThisNodeS(postProbSlice,
                    alphaSlice,
                    betaSlice,
                    funcVal,
                    labelSlice, 
                    posScoreSlice,
                    Inputs(2)->FunctionValues(),
                    mStartLbl, mEndLbl);

                FunctionValues() += funcVal;
            }
        }

        virtual void ComputeInputPartial(const size_t inputIndex)  //scaled by 2*number of colmns (samples) in the Matrix<ElemType>
        {
            if (inputIndex != 1 && inputIndex != 2)
                InvalidArgument("CRFNode only takes with respect to input and weight.");

            if (inputIndex == 1)
                ErrorSignalToPostitionDependentNode(GradientValues(), Inputs(0)->FunctionValues(), mPostProb, Inputs(inputIndex)->GradientValues());
            else if (inputIndex == 2)
            {
                size_t ncol = mAlpha.GetNumCols();
                size_t nstep = ncol / GetNumParallelSequences();
                assert(Inputs(inputIndex)->GradientValues().GetNumElements() > 0);
                for (size_t i = 0; i < GetNumParallelSequences(); i++)
                {
                    ErrorSignalToTransitionNode(
                        Inputs(0)->FunctionValues().ColumnSlice(i * nstep, nstep),
                        mAlpha.ColumnSlice(i * nstep, nstep),
                        mBeta.ColumnSlice(i * nstep, nstep),
                        Inputs(inputIndex)->FunctionValues(),
                        Inputs(inputIndex)->GradientValues(),
                        mStartLbl, 1);
                }
            }
            else
                return;
        }

        static void ErrorSignalToPostitionDependentNode(const Matrix<ElemType>& gradientValues, const Matrix<ElemType>& labls, const Matrix<ElemType>& postProb, Matrix<ElemType>& grd)
        {
            Matrix<ElemType>::AddScaledDifference(gradientValues, postProb, labls, grd);
        }

        static void ErrorSignalToTransitionNode(
            const Matrix<ElemType>& labls, const Matrix<ElemType>& alpha, const Matrix<ElemType>& beta,
            const Matrix<ElemType>& pair_scores, Matrix<ElemType>& grd,
            const int startLbl, const size_t shift = 1)
        {
            TransGrdCompute(labls,
                alpha,
                beta,
                pair_scores,
                grd,
                startLbl, shift);
        }

        /// compute forward backward algorithm
        static void EvaluateThisNodeS(Matrix<ElemType>& postprob, Matrix<ElemType>& alpha, Matrix<ElemType>& beta, Matrix<ElemType>& functionValues, const Matrix<ElemType>& lbls, const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores, int& firstLbl, int& lastLbl, const int iStep = 1)
        {
            /// to-do, each slice is for one sentence
            /// to-do, number of slices correspond to number of frames 
            /// this implementation only supports one sentence per minibatch

            int nObs = lbls.GetNumCols();

            /// change to other values so can support multiple sentences in each minibatch
            assert(iStep == 1);
            ForwardCompute(alpha, lbls, pos_scores, pair_scores);
            BackwardCompute(alpha, beta, functionValues, lbls, pos_scores, pair_scores, iStep);
            PostProbCompute(postprob, alpha, beta);

            firstLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, 0) != 0){
                firstLbl = ik; break;
            }

            lastLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, nObs - 1) != 0){
                lastLbl = ik; break;
            }

            functionValues.AssignInnerProductOfMatrices(lbls, pos_scores);

            Matrix<ElemType> a = alpha.ColumnSlice(nObs - 1, 1);
            ElemType fAlpha;
            fAlpha = a.LogAddSumOfElements();

            /// transition score
            ElemType tscore = 0;
            for (int t = 0; t < nObs - 1; t++){
                int i = -1;
                for (int ik = 0; ik < lbls.GetNumRows(); ik++)
                if (lbls(ik, t) != 0){
                    i = ik; break;
                }
                int j = -1;
                for (int ik = 0; ik < lbls.GetNumRows(); ik++)
                if (lbls(ik, t + 1) != 0){
                    j = ik; break;
                }
                tscore += pair_scores(j, i);
            }
            tscore += functionValues.Get00Element();  /// correct path score
            tscore -= fAlpha;  /// reduced by the scores from all paths
            functionValues.SetValue(tscore);

            functionValues *= (-1);
        }

        /// compute forward backward algorithm
        static void ForwardCompute(Matrix<ElemType>& alpha,
            const Matrix<ElemType>& lbls,
            const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores)
        {
            /// to-do, shift more than 1 to support muliple sentences per minibatch
            int iNumPos = lbls.GetNumCols();
            int iNumLab = lbls.GetNumRows();

            int firstLbl = -1;
            for (int ik = 0; ik < lbls.GetNumRows(); ik++)
            if (lbls(ik, 0) != 0){
                firstLbl = ik; break;
            }

            /// need to have 
            alpha.Resize(iNumLab, iNumPos);

            for (int t = 0; t < iNumPos; t++)
            {
                for (int k = 0; k < iNumLab; k++)
                {
                    ElemType fTmp = (ElemType)LZERO;
                    for (int j = 0; j < iNumLab; j++)
                    {
                        ElemType fAlpha = (j == firstLbl) ? (ElemType) 0.0 : (ElemType)LZERO;
                        if (t > 0)
                            fAlpha = alpha(j, t - 1);
                        fTmp = alpha.LogAdd(fTmp, fAlpha + pair_scores(k, j));
                    }
                    fTmp += pos_scores(k, t);  /// include position dependent score
                    alpha(k, t) = fTmp;
                }
            }
        }

        /// compute backward algorithm
        static void BackwardCompute( const Matrix<ElemType>& alpha, Matrix<ElemType>& beta,
            Matrix<ElemType>& functionValues, const Matrix<ElemType>& lbls,
            const Matrix<ElemType>& pos_scores, const Matrix<ElemType>& pair_scores, const int shift = 1)
        {
            assert(shift == 1);

            alpha.RCRFBackwardCompute(alpha, beta, functionValues, lbls, pos_scores, pair_scores, shift);
        }

        static void TransGrdCompute(const Matrix<ElemType>& lbls,
            const Matrix<ElemType>&   alpha,
            const Matrix<ElemType>& beta,
            const Matrix<ElemType>& pair_scores,
            Matrix<ElemType>& grd,
            const int startLbl,
            const int shift = 1)
        {
            assert(shift == 1);

            alpha.RCRFTransGrdCompute(lbls,
                alpha,
                beta,
                pair_scores,
                grd,
                startLbl, shift);
        }

        /// compute forward backward algorithm
        static void PostProbCompute(Matrix<ElemType>& postprob, const Matrix<ElemType>& alpha, const Matrix<ElemType>& beta)
        {
            int iNumPos = alpha.GetNumCols();
            int iNumLab = alpha.GetNumRows();

            postprob.Resize(iNumLab, iNumPos);
            postprob.SetValue(beta);
            postprob.InplaceExp();
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (!(Inputs(1)->FunctionValues().GetNumRows() == Inputs(2)->FunctionValues().GetNumRows() &&  // position dependent and pair scores have same number of labels
                Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows() &&
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols() && // position dependent and pair scores have the same observation numbers
                Inputs(2)->FunctionValues().GetNumCols() == Inputs(2)->FunctionValues().GetNumRows()))
            {
                LogicError("The Matrix<ElemType>  dimension in the CRFNode operation does not match.");
            }

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;
        }

        //virtual void AttachInputs(const ComputationNodePtr label,
        //    const ComputationNodePtr position_dependent_score,
        //    const ComputationNodePtr transition_score)
        //{
        //    m_children.resize(3);
        //    m_children[0] = label;
        //    m_children[1] = position_dependent_score;
        //    m_children[2] = transition_score;
        //}

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CRFNode<ElemType>>(nodeP);
                node->mAlpha = mAlpha;
                node->mBeta= mBeta;
                node->mPostProb = mPostProb;

                node->mStartLbl = mStartLbl;
                node->mEndLbl = mEndLbl;
            }
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    private:
        Matrix<ElemType> mAlpha;    // TODO: m_Alpha etc.
        Matrix<ElemType> mBeta;
        Matrix<ElemType> mPostProb;
        int mStartLbl;
        int mEndLbl;
    };

    // -----------------------------------------------------------------------
    /// DummyCriterionNode (objectives, derivatives, prediction)
    // -----------------------------------------------------------------------

    // This training criterion node needs derivatives and objectives to be
    // computed out of the node. Derivatives and objectives will be fed to the
    // node as input features. It has 3 inputs:
    // 1. feature node that feeds objectives
    // 2. feature node that feeds derivatives
    // 3. neural network output
    //
    // This node is useful in sequence training for speech recognition, so that
    // we can separate lattice computation (which may rely other softwares, such
    // as Kaldi) with the neural network training.
    template<class ElemType>
    class DummyCriterionNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<3>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"DummyCriterion"; }
    public:
        DummyCriterionNode(DEVICEID_TYPE deviceId, const wstring & name) :
          Base(deviceId, name)
        { }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 2)
                InvalidArgument("DummyCriterionNode only takes three inputs.");
            else if (inputIndex == 0)
                LogicError("DummyCriterionNode: derivatives with respect to objective features are not necessary, not implemented yet.\n");
            else if (inputIndex == 1)
                LogicError("DummyCriterionNode: derivatives with respect to derivative features are not necessary, not implemented yet.\n");
            else
                ComputeInputPartialThree(Inputs(1)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues());
        }

        /*TODO: merge with call site*/void ComputeInputPartialThree(const Matrix<ElemType>& inputFunctionValues1,
                                                    Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        {
            Matrix<ElemType>::ScaleAndAdd(gradientValues.Get00Element(), inputFunctionValues1, inputGradientValues);
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0)  
        {
            if (inputFunctionValues0.GetNumRows() != 1 || inputFunctionValues0.GetNumCols() != 1)
                LogicError("DummyCriterionNode expects first input has dimension (1, 1).\n");
            functionValues.Resize(1, 1);
            functionValues.SetValue(inputFunctionValues0.Get00Element());
#if NANCHECK
            functionValues.HasNan("DummyCriterionNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate() override
        {
            Base::Validate();

            if (Inputs(0)->OperationName() != L"InputValue")
                LogicError("DummyCriterionNode criterion requires the first input to be computed objectives.");
            if (Inputs(0)->OperationName() != L"InputValue")
                LogicError("DummyCriterionNode criterion requires the first input to be computed derivatives.");
            if (Inputs(0)->FunctionValues().GetNumRows() != 1)
                LogicError("DummyCriterionNode criterion requires the first input to have dimension 1.");
            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements() || Inputs(2)->FunctionValues().HasNoElements())
                LogicError("DummyCriterionNode operation: one of the operants has 0 element.");
            if (Inputs(1)->FunctionValues().GetNumRows() != Inputs(2)->FunctionValues().GetNumRows())
                LogicError("The Matrix dimension in the DummyCriterionNode operation does not match.");
            if (Inputs(1)->FunctionValues().GetNumCols() != Inputs(2)->FunctionValues().GetNumCols())
                Inputs(1)->FunctionValues().Resize(Inputs(1)->FunctionValues().GetNumRows(), Inputs(2)->FunctionValues().GetNumCols()); 

            FunctionValues().Resize(1,1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;        
        }

        //virtual void AttachInputs(const ComputationNodePtr objectives, const ComputationNodePtr derivatives, const ComputationNodePtr prediction) 
        //{
        //    m_children.resize(3);
        //    m_children[0] = objectives;
        //    m_children[1] = derivatives;
        //    m_children[2] = prediction;
        //}
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
    };

    template class DummyCriterionNode<float>; 
    template class DummyCriterionNode<double>;

    //discriminative sequence training criteria 
    template<class ElemType>
    class SequenceWithSoftmaxNode : public ComputationNodeNonLooping<ElemType>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SequenceWithSoftmax"; }
    public:
        SequenceWithSoftmaxNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_logSoftmaxOfRight(deviceId), m_softmaxOfRight(deviceId), m_gammaFromLattice(deviceId), m_maskOfFramedrop(deviceId), m_gammaCalcInitialized(false)
        {
        }
        
        //compute gradients to input observations, the weights to the observations, and the class log posterior probabilites
        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            //auto t_start_time = Timer::MilliSecondElapsed();
            if (inputIndex > 2)
                InvalidArgument("SequenceWithSoftmaxNode criterion only takes three inputs.");

            //left Node must be a scalar
            if (inputIndex == 0)  //left derivative
            {
                ComputeInputPartialLeft(m_logSoftmaxOfRight, Inputs(inputIndex)->GradientValues(), GradientValues());
            }
            else if (inputIndex == 1)
            {
                ComputeInputPartialRight(m_softmaxOfRight, Inputs(0)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues(), m_gammaFromLattice,
                    m_hsmoothingWeight, m_frameDropThresh);
                Base::MaskMissingColumnsToZero(Inputs(inputIndex)->GradientValues());
            }
            else if (inputIndex == 2)
            {
                Inputs(inputIndex)->NeedGradient() = false;
                Inputs(inputIndex)->GradientValues().SetValue(0.0);
            }
            else
                throw std::runtime_error("SequenceWithSoftmaxNode criterion only takes with respect to label, DNN output and log likelihood.");
        }

        
        static void WINAPI ComputeInputPartialLeft(const Matrix<ElemType>& logSoftmaxOfRight, Matrix<ElemType>& inputGradientValues,
            const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT
            logSoftmaxOfRight.Print("SequenceWithSoftmaxNode Partial-logSoftmaxOfRight");
            gradientValues.Print("SequenceWithSoftmaxNode Partial-gradientValues");
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Left-in");
#endif

            Matrix<ElemType>::ScaleAndAdd(-gradientValues.Get00Element(), logSoftmaxOfRight, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Left-out");
#endif

        }

        static void WINAPI ComputeInputPartialRight(const Matrix<ElemType>& softmaxOfRight, const Matrix<ElemType>& inputFunctionValues,
            Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const Matrix<ElemType> &gammaFromLattice,
            const ElemType &hsmoothingWeight, const ElemType &frameDropThresh)
        {
#if DUMPOUTPUT
            softmaxOfRight.Print("SequenceWithSoftmaxNode Partial-softmaxOfRight");
            inputFunctionValues.Print("SequenceWithSoftmaxNode Partial-inputFunctionValues");
            gradientValues.Print("SequenceWithSoftmaxNode Partial-gradientValues");
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Right-in");
#endif  
            
            inputGradientValues.AssignSequenceError(hsmoothingWeight, inputFunctionValues, softmaxOfRight, gammaFromLattice, gradientValues.Get00Element());            
            inputGradientValues.DropFrame(inputFunctionValues, gammaFromLattice, frameDropThresh);    
            

#if DUMPOUTPUT
            inputGradientValues.Print("SequenceWithSoftmaxNode Partial-Right");
#endif
        }

        virtual void EvaluateThisNodeNonLooping()   //-sum(left_i * log(softmax_i(right)))
        {
            // Initialize m_GammaCal
            if (!m_gammaCalcInitialized)
            {
                if (m_hmm.hmms.size() == 0)
                {
                    LogicError("SequenceWithSoftmaxNode criterion evaluation requires HMM states to be set.");
                }
                m_GammaCal.init(m_hmm, m_deviceId);
                m_gammaCalcInitialized = true;
            }
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), m_softmaxOfRight, m_logSoftmaxOfRight, m_gammaFromLattice, m_lattice, m_GammaCal,
                m_uids, m_boundaries,  m_pMBLayout, m_extrauttmap, m_doreferencealign);
        }

        
        static void WINAPI EvaluateThisNodeS(Matrix<ElemType>& functionValues, Matrix<ElemType>& inputFunctionValues0, Matrix<ElemType>& inputFunctionValues1,
            const Matrix<ElemType>& inputFunctionValues2, Matrix<ElemType>& softmaxOfRight, Matrix<ElemType>& logSoftmaxOfRight, Matrix<ElemType>& gammafromlattice,
            std::vector<shared_ptr<const msra::dbn::latticesource::latticepair>> &lattices, msra::lattices::GammaCalculation<ElemType> &GammaCal, std::vector<size_t> & uids,
            std::vector<size_t> & boundaries,  MBLayoutPtr pMBLayout, std::vector<size_t> &extrauttmap, bool doReferenceAlign)
        {
            //softmax 
            logSoftmaxOfRight.AssignLogSoftmaxOf(inputFunctionValues1, true);
            softmaxOfRight.SetValue(logSoftmaxOfRight);
            softmaxOfRight.InplaceExp();

            size_t sequenceNum = pMBLayout->GetNumParallelSequences();
            gammafromlattice.SwitchToMatrixType(softmaxOfRight.GetMatrixType(), softmaxOfRight.GetFormat(), false);
            gammafromlattice.Resize(softmaxOfRight.GetNumRows(), softmaxOfRight.GetNumCols());
            GammaCal.calgammaformb(functionValues, lattices, inputFunctionValues2, inputFunctionValues0, gammafromlattice, uids, boundaries, sequenceNum, pMBLayout, extrauttmap, doReferenceAlign);
            
#if NANCHECK
            functionValues.HasNan("SequenceWithSoftmaxNode");
#endif
#if DUMPOUTPUT
            functionValues.Print("SequenceWithSoftmaxNode");
#endif
        }

        virtual void Validate()
        {
            Base::Validate();

            if (m_children.size() != 3)
                LogicError("SequenceWithSoftmaxNode criterion requires three inputs.");

            if (Inputs(0)->OperationName() != L"InputValue" && Inputs(0)->OperationName() != L"SparseInputValue")
                LogicError("SequenceWithSoftmaxNode criterion requires the first input to be the label.");

            //we may release the constraint that the first operant is an inputValue later so the following code should be kept
            size_t index = 0;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0 ? Inputs(1 - index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0 ? Inputs(1 - index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            index = 1;
            if (Inputs(index)->OperationName() == OperationNameOf(LearnableParameter))
            {
                size_t rows = Inputs(index)->FunctionValues().GetNumRows() == 0 ? Inputs(1 - index)->FunctionValues().GetNumRows() : Inputs(index)->FunctionValues().GetNumRows();
                size_t cols = Inputs(index)->FunctionValues().GetNumCols() == 0 ? Inputs(1 - index)->FunctionValues().GetNumCols() : Inputs(index)->FunctionValues().GetNumCols();
                Inputs(index)->FunctionValues().Resize(rows, cols);
            }

            if (Inputs(0)->FunctionValues().HasNoElements() || Inputs(1)->FunctionValues().HasNoElements()  || Inputs(2)->FunctionValues().HasNoElements() )
                LogicError("SequenceWithSoftmaxNode operation: one of the operants has 0 element.");

            if (!(Inputs(0)->FunctionValues().GetNumRows() == Inputs(1)->FunctionValues().GetNumRows() &&  //match size
                Inputs(1)->FunctionValues().GetNumRows() == Inputs(2)->FunctionValues().GetNumRows() &&
                Inputs(0)->FunctionValues().GetNumCols() == Inputs(1)->FunctionValues().GetNumCols() &&
                Inputs(1)->FunctionValues().GetNumCols() == Inputs(2)->FunctionValues().GetNumCols()))
            {
                LogicError("The Matrix<ElemType>  dimension in the SequenceWithSoftmaxNode operation does not match.");
            }

            FunctionValues().Resize(1, 1);
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();

            m_logSoftmaxOfRight.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_softmaxOfRight.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_gammaFromLattice.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_maskOfFramedrop.Resize(Inputs(0)->FunctionValues().GetNumRows(), Inputs(0)->FunctionValues().GetNumCols());
            m_gammatime = 0;
            m_partialtime = 0;
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputChannels = 1;
            m_outputWidth = 1;
            m_outputHeight = 1;
        }

        //leftNode should be the empirical
        virtual void AttachInputs(const ComputationNodePtr label, const ComputationNodePtr prediction, const ComputationNodePtr loglikelihood)
        {
            m_children.resize(3);
            m_children[0] = label;
            m_children[1] = prediction;
            m_children[2] = loglikelihood;
            loglikelihood->NeedGradient() = false;
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);            
            m_logSoftmaxOfRight.TransferToDeviceIfNotThereAndNotAutoPlace( deviceId, true);
            m_softmaxOfRight.TransferToDeviceIfNotThereAndNotAutoPlace( deviceId, true);
            m_gammaFromLattice.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
            m_maskOfFramedrop.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);            
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            Base::CopyTo(nodeP, newName, flags);
            
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<SequenceWithSoftmaxNode<ElemType>>(nodeP);

                node->m_logSoftmaxOfRight = m_logSoftmaxOfRight;
                node->m_softmaxOfRight = m_softmaxOfRight;
                node->m_gammaFromLattice = m_gammaFromLattice;
                node->m_maskOfFramedrop = m_maskOfFramedrop;
                node->m_hsmoothingWeight = m_hsmoothingWeight;
                node->m_frameDropThresh = m_frameDropThresh;
                node->m_doreferencealign = m_doreferencealign;
            }
        }

        std::vector<shared_ptr<const msra::dbn::latticesource::latticepair>> * getLatticePtr()
        {
            return &m_lattice;
        }

        std::vector<size_t> * getuidprt()
        {
            return &m_uids;
        }

        std::vector<size_t> * getboundaryprt()
        {
            return &m_boundaries;
        }
        std::vector<size_t> * getextrauttmap()
        {
            return &m_extrauttmap;
        }
        msra::asr::simplesenonehmm *gethmm()
        {
            return &m_hmm;
        }

        void SetSmoothWeight(const ElemType hsmoothingWeight)
        {
            m_hsmoothingWeight = hsmoothingWeight;
        }
        void SetFrameDropThresh(const ElemType frameDropThresh)
        {
            m_frameDropThresh = frameDropThresh;
        }

        void SetReferenceAlign(const bool dorefrencealign)
        {
            m_doreferencealign = dorefrencealign;
        }
        void gettime(unsigned long long &gammatime, unsigned long long &partialtime)
        {
            gammatime = m_gammatime;
            partialtime = m_partialtime;
        }
    protected:
        virtual bool NodeDoesItsOwnCustomizedMissingColumnsMasking() { return true; }
        Matrix<ElemType> m_logSoftmaxOfRight;
        Matrix<ElemType> m_softmaxOfRight;
        Matrix<ElemType> m_gammaFromLattice;
        Matrix<ElemType> m_maskOfFramedrop;
        ElemType m_frameDropThresh;
        ElemType m_hsmoothingWeight;
        bool m_doreferencealign;
        std::vector<shared_ptr<const msra::dbn::latticesource::latticepair>> m_lattice;
        msra::asr::simplesenonehmm m_hmm;
        msra::lattices::GammaCalculation<ElemType> m_GammaCal;
        bool m_gammaCalcInitialized;
        std::vector<size_t> m_uids;
        std::vector<size_t> m_boundaries;
        std::vector<size_t> m_extrauttmap;

        unsigned long long m_gammatime;
        unsigned long long m_partialtime;
    };



    template class SequenceWithSoftmaxNode<float>;
    template class SequenceWithSoftmaxNode<double>;
}}}