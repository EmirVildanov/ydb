#pragma once

#include "abstract.h"
#include "gorilla_inner.h"

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <ydb/library/accessor/accessor.h>
#include <ydb/library/conclusion/result.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/options.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>

namespace NKikimr::NArrow::NSerialization {

    class TGorillaSerializer: public ISerializer {
    public:
        static TString GetClassNameStatic() {
            return "ARROW_GORILLA_SERIALIZER";
        }
    private:
        Compressor compressor;
        Decompressor compressor;
        arrow::ipc::IpcWriteOptions Options;

        TConclusion<std::shared_ptr<arrow::util::Codec>> BuildCodec(const arrow::Compression::type& cType, const std::optional<ui32> level) const;
        static const inline TFactory::TRegistrator<TGorillaSerializer> Registrator = TFactory::TRegistrator<TGorillaSerializer>(GetClassNameStatic());
    protected:
        virtual bool IsCompatibleForExchangeWithSameClass(const ISerializer& /*item*/) const override {
            return true;
        }

        virtual bool IsEqualToSameClass(const ISerializer& item) const override {
            auto& itemOptions = static_cast<const TGorillaSerializer&>(item).Options;
            if (!!itemOptions.codec != !!Options.codec) {
                return false;
            }
            if (!itemOptions.codec) {
                return true;
            }
            if (itemOptions.codec->name() != Options.codec->name()) {
                return false;
            }
            if (itemOptions.codec->compression_level() != Options.codec->compression_level()) {
                return false;
            }
            return true;
        }
        virtual TString DoSerializeFull(const std::shared_ptr<arrow::RecordBatch>& batch) const override;
        virtual TString DoSerializePayload(const std::shared_ptr<arrow::RecordBatch>& batch) const override;
        virtual arrow::Result<std::shared_ptr<arrow::RecordBatch>> DoDeserialize(const TString& data) const override;
        virtual arrow::Result<std::shared_ptr<arrow::RecordBatch>> DoDeserialize(const TString& data, const std::shared_ptr<arrow::Schema>& schema) const override;

        virtual TConclusionStatus DoDeserializeFromRequest(NYql::TFeaturesExtractor& features) override;

        static arrow::ipc::IpcOptions BuildDefaultOptions() {
            arrow::ipc::IpcWriteOptions options;
            options.use_threads = false;
            options.codec = *arrow::util::Codec::Create(arrow::Compression::LZ4_FRAME);
            return options;
        }

        virtual TConclusionStatus DoDeserializeFromProto(const NKikimrSchemeOp::TOlapColumn::TSerializer& proto) override;

        virtual void DoSerializeToProto(NKikimrSchemeOp::TOlapColumn::TSerializer& proto) const override;

    public:
        virtual TString GetClassName() const override {
            return GetClassNameStatic();
        }

        virtual bool IsHardPacker() const override {
            return Options.codec && Options.codec->compression_type() == arrow::Compression::ZSTD && Options.codec->compression_level() > 3;
        }

        static arrow::ipc::IpcOptions GetDefaultOptions() {
            static arrow::ipc::IpcWriteOptions options = BuildDefaultOptions();
            return options;
        }

        TGorillaSerializer(const arrow::Compression::type compressionType) {
            Options.use_threads = false;
            auto r = arrow::util::Codec::Create(compressionType);
            AFL_VERIFY(r.ok());
            Options.codec = std::move(*r);
        }

        TGorillaSerializer(const arrow::ipc::IpcWriteOptions& options = GetDefaultOptions())
                : Options(options) {
            Options.use_threads = false;

        }
    };

}
