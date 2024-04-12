/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <miopen/graphapi/graphapi.hpp>

#include <miopen/graphapi/tensor.hpp>

namespace miopen {
namespace graphapi {

class Matmul
{
private:
    miopenDataType_t mCompType;

public:
    Matmul() = default;
    Matmul(miopenDataType_t computeType) : mCompType(computeType) {}
    miopenDataType_t getComputeType() { return mCompType; }

private:
    friend class MatmulBuilder;
};

class MatmulBuilder
{

private:
    Matmul mMatmul;
    bool mComputeTypeSet = false;

public:
    MatmulBuilder& setComputeType(miopenDataType_t computeType)
    {
        mMatmul.mCompType = computeType;
        mComputeTypeSet   = true;
        return *this;
    }

    Matmul build() const;
};

class BackendMatmulDescriptor : public BackendDescriptor
{
private:
    MatmulBuilder mBuilder;
    Matmul mMatmul;

public:
    virtual void setAttribute(miopenBackendAttributeName_t attributeName,
                              miopenBackendAttributeType_t attributeType,
                              int64_t elementCount,
                              void* arrayOfElements) override;
    virtual void finalize() override;
    virtual void getAttribute(miopenBackendAttributeName_t attributeName,
                              miopenBackendAttributeType_t attributeType,
                              int64_t requestedElementCount,
                              int64_t* elementCount,
                              void* arrayOfElements) override;

    const Matmul* getMatmul() const noexcept { return &mMatmul; }
    Matmul* getMatmul() noexcept { return &mMatmul; }
};

Class OperationMatmul {
    private:
    BackendTensorDescriptor mA;
    BackendTensorDescriptor mB;
    BackendTensorDescriptor mC;
    int64_t mBatchCount = 1;
    BackendTensorDescriptor mGemmMOverride;
    BackendTensorDescriptor mGemmNOverride;
    BackendTensorDescriptor mGemmKOverride;
    BackendMatmulDescriptor mMatmul;
    public:
    OperationMatmul() = default;
    BackendTensorDescriptor getA() { return mA};
    BackendTensorDescriptor getB() { return mB};
    BackendTensorDescriptor getC() { return mC};
    int64_t getBatchCount() { return mBatchCount};
    BackendTensorDescriptor getMOverride() { return mGemmMOverride};
    BackendTensorDescriptor getNOverride() { return mGemmNOverride};
    BackendTensorDescriptor getKOverride() { return mGemmKOverride};
    BackendMatmulDescriptor getMatmul() { return mMatmul};
    private:
    friend class BackendOperationMatmulBuilder;
}

class BackendOperationMatmulBuilder {
    private:
    bool mMatmulSet = false;
    bool mASet = false;
    bool mBSet = false;
    bool mCSet = false;
    OperationMatmul mMatmul;
    public:
    BackendOperationMatmulBuilder &setA(BackendTensorDescriptor A) {
       mMatmul.mA = A;
       mASet = true;
       return *this;
    };
    BackendOperationMatmulBuilder &setB(BackendTensorDescriptor B) {
       mMatmul.mB = B;
       mBSet = false;
       return *this;
    };
    BackendOperationMatmulBuilder &setC(BackendTensorDescriptor C) {
       mMatmul.mC = C;
       mCSet = false;
       return *this;
    };
    BackendOperationMatmulBuilder &setBatchCount(int64_t count) {
       mMatmul.mBatchCount = count;
       return *this;
    };
    BackendTensorDescriptor &setGemmMOverride(BackendTensorDescriptor overrideTensor) {
        mMatmul.mGemmMOverride = overrideTensor;
        return *this;
    };
    BackendTensorDescriptor &setGemmNOverride(BackendTensorDescriptor overrideTensor) {
        mMatmul.mGemmNOverride = overrideTensor;
        return *this;
    };
    BackendTensorDescriptor &setGemmKOverride(BackendTensorDescriptor overrideTensor) {
        mMatmul.mGemmKOverride = overrideTensor;
        return *this;
    };
    BackendTensorDescriptor &setMatmulDescriptor(BackendMatmulDescriptor matmul) {
        mMatmul = matmul;
        mMatmulSet = true;
        return *this;
    }
    OperationMatmul build();
}

class BackendOperationMatmul : public BackendDescriptor
{
    private:
    BackendOperationMatmulBuilder mBuilder;
    OperationMatmul mMatmul;
    public:
    virtual void setAttribute(miopenBackendAttributeName_t attributeName,
                              miopenBackendAttributeType_t attributeType,
                              int64_t elementCount,
                              void* arrayOfElements) override;
    virtual void finalize() override;
    virtual void getAttribute(miopenBackendAttributeName_t attributeName,
                              miopenBackendAttributeType_t attributeType,
                              int64_t requestedElementCount,
                              int64_t* elementCount,
                              void* arrayOfElements) override;
}

} // namespace graphapi
} // namespace miopen
