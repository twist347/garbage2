#ifndef DM_PDM_SERVICE_HPP
#define DM_PDM_SERVICE_HPP

#include <functional>
#include <random>
#include <optional>
#include <boost/core/ignore_unused.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/serialization/singleton.hpp>
#include <boost/utility/string_view.hpp>
#include "pdm-roles.hpp"
#include <wi-rpc-dto.hpp>
#include <iwi-session.hpp>
#include <internal/wi-cache-helper.hpp>
#include <data-access-router.hpp>
#include <pdm-error.hpp>
#include "wi-reliability-dto.hpp"
#include <wi-dto-applies.hpp>
#include <internal/users-service.hpp>
#include "context/transaction-guard.hpp"
#include <wi-numerator.hpp>

namespace net = boost::asio;
using namespace wi::core;
using namespace wi::core::platform;
using namespace wi::core::cache;
using namespace wi::basic_services::pdm::error;
using namespace wi::basic_services;
using namespace std::chrono_literals;

#define PdmSvc wi::basic_services::pdm::internal::PdmService::get_mutable_instance()
#define PdmSvcConst wi::basic_services::pdm::internal::PdmService::get_const_instance()

#define GUARD_PDM_METHOD_PUB() MethodGuard mctx(ctx,ec,yield,std::chrono::milliseconds(WI_CONFIGURATION().read_settings<size_t>(server_method_timeout)))

namespace wi::basic_services::pdm::internal {
    namespace details {
            struct Node{
                Node(Number failure_rate):fr(failure_rate){}
                BOOST_HANA_DEFINE_STRUCT(Node,
                                         (Number, fr));
            };
            struct LinearModel;
            struct ReservedModel;
            using LinearModelPtr = std::unique_ptr<LinearModel>;
            using ReservedModelPtr = std::unique_ptr<ReservedModel>;
            using RBDPartModel = std::variant<Node,LinearModelPtr,ReservedModelPtr>;
            struct LinearModel{
                BOOST_HANA_DEFINE_STRUCT(LinearModel,
                                         (std::vector<RBDPartModel>, chain));
            };
            struct ReservedModel{
                BOOST_HANA_DEFINE_STRUCT(ReservedModel,(std::vector<RBDPartModel>, chains));
            };
    }
class PdmService: public boost::noncopyable, public boost::serialization::singleton<PdmService> {
    public:

        PdmService();
    public:

        void init(IWiPlatform::PlatformEventNumeratorPtr numerator, net::io_context &ios, boost::system::error_code &ec, const net::yield_context &yield);
        void addNewNode(std::size_t initiatingService, WiNewPdmNodeQuery &query, const std::shared_ptr<IWiSession> sessionPtr, const Filter &filter, boost::system::error_code &ec, const net::yield_context &yield, std::shared_ptr<MethodContextInterface> ctx = nullptr) const;
        void updateNode(std::size_t initiatingService, WiUpdatePdmNodeQuery &query, const std::shared_ptr<IWiSession> sessionPtr, const Filter &filter, boost::system::error_code &ec, const net::yield_context &yield, std::shared_ptr<MethodContextInterface> ctx = nullptr) const;
        void deleteNode(std::size_t initiatingService, const std::string &query, const std::shared_ptr<IWiSession> sessionPtr, const Filter &filter, boost::system::error_code &ec, const net::yield_context &yield, std::shared_ptr<MethodContextInterface> ctx = nullptr) const;

        // new context methods
        std::string addNewNode(WiNewPdmNodeQuery &query, std::shared_ptr<WiSessionContext<IWiSession>> sessionContext, const Filter &filter, const net::yield_context &yield) const;
        void updateNode(WiUpdatePdmNodeQuery &query, std::shared_ptr<WiSessionContext<IWiSession>> sessionContext, const Filter &filter, const net::yield_context &yield) const;
        void deleteNode(const std::string &semantic, std::shared_ptr<WiSessionContext<IWiSession>> sessionContext, const Filter &filter, const net::yield_context &yield) const;

        std::shared_ptr<WiPdmRawNode> fetchRawNode(const std::string &semantic, std::shared_ptr<WiSessionContext<IWiSession>> sessionContext, const net::yield_context &yield) const noexcept(true);
        std::shared_ptr<WiPdmRawNodeEntity> fetchRawNodeEntity(const std::string &semantic, std::shared_ptr<WiSessionContext<IWiSession>> sessionContext, const net::yield_context &yield) const noexcept(true);

    private:

        void initiateRecalculation(
            std::int64_t initiatingService,
            const std::shared_ptr<IWiSession> sessionPtr,
            const std::string &initiator,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        void recalculateProductFullElement(
            std::int64_t initiatingService,
            const WiPdmRawNodeEntity &element,
            const std::optional<long double> timespan,
            bool reset,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateProductFullLayer(
            std::int64_t initiatingService,
            const WiPdmRawNodeEntity &element,
            const std::optional<long double> timespan,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateProductFull(
            std::int64_t initiatingService,
            const std::string &product,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateRestoredElement(
            std::int64_t initiatingService,
            const std::string &product,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateProduct(
            std::int64_t initiatingService,
            const std::string &product,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        // fixme: deprecated
        void recalculateRbds(
            std::int64_t initiatingService,
            const std::string &rbds,
            const std::shared_ptr<IWiSession> sessionPtr,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateRbd(
            std::int64_t initiatingService,
            const std::string &rbd,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void recalculateContainer(
            std::int64_t initiatingService,
            const std::string &rbd,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        void deleteElementInternal_impl(
            std::size_t initiatingService,
            const WiPdmRawNode &node,
            WiPdmElementData&prod_data,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        std::optional<WiSemanticResult> addProject(
                std::size_t initiatingService,
                const WiNewProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteElementInternal(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteElementsFromBin(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> deleteElementsToBin(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteAllElementsFromBin(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteElementsDeprecated(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteProject(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmElementVariables> getRbdBlockElementVars(
                std::size_t initiatingService,
                std::shared_ptr<WiPdmRawNodeEntity> block,
                const std::optional<long double>&timespan,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<WiPdmElementVariables> getSubRbdRefSchemaVars(
                std::size_t initiatingService,
                std::shared_ptr<WiPdmRawNodeEntity> sub,
                const std::optional<long double>&timespan,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<details::ReservedModelPtr> getRbdReservedModel(
                std::size_t initiatingService,
                const WiRbdChain &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<details::LinearModelPtr> getRbdChainModel(
                std::size_t initiatingService,
                const WiRbdChain &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<WiSemanticResult> addProduct(
                std::size_t initiatingService,
                const WiNewProduct &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void provisionRbdFlags(
                std::size_t initiatingService,
                const std::string &schema,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
        std::optional<WiSemanticResult> addRbd(
                std::size_t initiatingService,
                const WiNewRbd &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiFmeaTemplate::Container> fetchFmeasTemplates(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addFmeaSheet(
                std::size_t initiatingService,
                const WiAddFmeaSheetQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteFmeaSheet(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiFmeasSheet::Container> fetchFmeasSheetsView(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        void addFmeaModule(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiRbdChain> copyRbdChainInternal(
                std::size_t initiatingService,
                const std::string& target_schema_semantic,
                const WiRbdChain &chain,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<WiRbdChain> copyRbdGroupInternal(
                std::size_t initiatingService,
                const std::string& target_schema_semantic,
                const WiRbdChain &group,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<WiSemanticResult> copyRbdBlockElement(
                std::size_t initiatingService,
                const std::string& target_schema_semantic,
                const std::string& source_semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void moveRbdRefs(
                std::size_t initiatingService,
                const std::string& source_rbd_semantic,
                const std::string& target_rbd_semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void assignRbdLinkSourceOutput(
                std::size_t initiatingService,
                const std::string &source,
                std::optional<std::string> &output,
                const std::string &target,
                const bool overwriteLinks,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void assignRbdLinkTargetInput(
                std::size_t initiatingService,
                const std::string &source,
                std::optional<std::string> &input,
                const std::string &target,
                const bool overwriteLinks,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void appendRbdLinkSourceOutputs(
                std::size_t initiatingService,
                const std::string &source,
                std::vector<std::string> &outputs,
                const std::string &target,
                const bool overwriteLinks,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void appendRbdLinkTargetInputs(
                std::size_t initiatingService,
                const std::string &source,
                std::vector<std::string> &inputs,
                const std::string &target,
                const bool overwriteLinks,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void insertEdgeElementInsideParallelConnection(
                const std::string &source,
                bool forward,
                const WiRbdChain &chain,
                const WiRbdLink &link,
                std::size_t initiatingService,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept;

        void linkRbdElements(
                std::size_t initiatingService,
                std::shared_ptr<WiPdmRawNodeEntity> source,
                std::shared_ptr<WiPdmRawNodeEntity> target,
                const bool overwriteLinks,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void addRbdLink(
                std::size_t initiatingService,
                const WiRbdLink &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void resetRbdLinkSourceOutput(
                std::size_t initiatingService,
                const std::string &source,
                std::optional<std::string> &output,
                const std::string &target,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void resetRbdLinkTargetInput(
                std::size_t initiatingService,
                const std::string &source,
                std::optional<std::string> &input,
                const std::string &target,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void removeRbdLinkSourceOutputs(
                std::size_t initiatingService,
                const std::string &source,
                std::vector<std::string> &outputs,
                const std::string &target,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void removeRbdLinkTargetInputs(
                std::size_t initiatingService,
                const std::string &source,
                std::vector<std::string> &inputs,
                const std::string &target,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void unlinkRbdElements(
                std::size_t initiatingService,
                std::shared_ptr<WiPdmRawNodeEntity> source,
                std::shared_ptr<WiPdmRawNodeEntity> target,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void removeRbdLink(
                std::size_t initiatingService,
                const WiRbdLink &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
    public:
        std::optional<WiSemanticResult> addRbdBlock(
                std::size_t initiatingService,
                const WiNewRbdBlock &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addSubRbd(
                std::size_t initiatingService,
                WiAddSubRbdQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteRbdBlocks(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);


    private:
        void deleteRbdBlock(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteSubRbd(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        void updateRbdBlockData(
                std::size_t initiatingService,
                const WiUpdateRbdBlockData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
    private:
        void updateRbdBlockDataInternal(std::size_t initiatingService,
                const WiUpdateRbdBlockData &query,
                std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUnbinding,
                std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUpdating,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void updateSubRbdDataInternal(std::size_t initiatingService,
                const WiUpdateRbdBlockData &query,
                std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUnbinding,
                std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUpdating,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void getRbdElementInput(
                std::size_t initiatingService,
                const std::shared_ptr<WiPdmRawNodeEntity> &element,
                std::optional<std::string>& input,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void getRbdElementInputs(
                std::size_t initiatingService,
                const std::shared_ptr<WiPdmRawNodeEntity> &element,
                std::vector<std::string>& input,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void getRbdElementOutput(
                std::size_t initiatingService,
                const std::shared_ptr<WiPdmRawNodeEntity> &element,
                std::optional<std::string>& output,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void getRbdElementOutputs(
                std::size_t initiatingService,
                const std::shared_ptr<WiPdmRawNodeEntity> &element,
                std::vector<std::string>& outputs,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void validateRbdGroup(
                std::size_t initiatingService,
                const WiRbdChain &group,
                bool validate,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void validateRbdChain(
                std::size_t initiatingService,
                const WiRbdChain &chain,
                bool open,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void checkRbdLinkExists(
                std::size_t initiatingService,
                const WiRbdLink &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void filloutRbdBlockCopyCreationQuery(
                std::size_t initiatingService,
                WiNewRbdBlock &query,
                const std::string &copy,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void filloutSubRbdCopyCreationQuery(
                std::size_t initiatingService,
                WiAddSubRbdQuery &query,
                const std::string &copy,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void validateRbdBlocksPartQuery(
                const WiNewRbdBlocksPart &query,
                boost::system::error_code &ec) const noexcept(true);

        void validateSubsRbdPartQuery(
                const WiNewSubsRbdPart &query,
                boost::system::error_code &ec) const noexcept(true);

    public:
        std::optional<WiRbdChain> addRbdBlockChain(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewRbdBlocksPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        
        std::optional<WiRbdChain> addSubRbdChain(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
    private:
        void addRbdBlocksToGroup(
                std::size_t initiatingService,
                const WiRbdChain &group,
                const WiNewRbdBlocksPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void addSubsRbdToGroup(
                std::size_t initiatingService,
                const WiRbdChain &group,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        std::optional<WiRbdChain> addRbdBlockGroup(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewRbdBlocksPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiRbdChain> addSubRbdGroup(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        std::optional<WiRbdChain> addRbdBlocks(
                std::size_t initiatingService,
                const WiNewRbdBlocks &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiRbdChain> addSubsRbd(
                std::size_t initiatingService,
                const WiNewSubsRbd &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        std::optional<WiSemanticsResult> addRbdBlocksInternal(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewRbdBlocksPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        void validateInsertRbdBesideQuery(
                const WiRbdInsertBeside &query,
                boost::system::error_code &ec) const noexcept(true);


        std::optional<WiSemanticsResult> addSubsRbdInternal(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);
        

    private:
        void insertRbdBetween(
                std::size_t initiatingService,
                const WiRbdLink &link,
                const WiRbdChain &chain,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        void insertRbdBeside(
            std::size_t initiatingService,
            const WiRbdInsertBeside &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        void validateInsertRbdInParallelQuery(
                const WiRbdInsertInParallel &query,
                boost::system::error_code &ec) const noexcept(true);

    public:
        void insertRbdInParallel(
                std::size_t initiatingService,
                const WiRbdInsertInParallel &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void insertRbdIntoParallel(
                std::size_t initiatingService,
                const WiInsertRdbIntoParallel &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        void changeElementFailureTypes(
                std::size_t initiatingService,
                const std::string &elementSemantic,
                const WiFailureTypeQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void removeElementFailureTypes(
                std::size_t initiatingService,
                const std::string &elementSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept;

        void updateElementSemanticInFailureTypeCache(
                std::size_t initiatingService,
                const std::string &oldSemantic,
                const std::string &newSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept;

    private:

        std::optional<WiRbdChain> createRbdGroup(
                std::size_t initiatingService,
                const std::string &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        void detachRbdChain(
                std::size_t initiatingService,
                const WiRbdChain &query,
                bool passthrough,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        std::optional<WiRbdChain> getRbdGroup(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void validateInsertRbdIntoGroupQuery(
                const WiRbdInsertIntoGroup &query,
                boost::system::error_code &ec) const noexcept(true);

        void insertRbdChainIntoGroup(
                std::size_t initiatingService,
                const WiRbdChain &group,
                const WiRbdChain &chain,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    public:
        void insertRbdIntoGroup(
                std::size_t initiatingService,
                const WiRbdInsertIntoGroup &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void BindPdmComponentWithRbdBlock(
                std::size_t initiatingService,
                const WiSemanticsPairQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void bindRbdSchemaWithSubRbd(
                std::size_t initiatingService,
                const WiSemanticsPairQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

    private:
        void unbindAllDescendantsFromRbd(
                std::size_t initiatingService,
                const std::string &element,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        std::optional<std::vector<std::string>> removeRbdBindingFromElement(
                std::size_t initiatingService,
                const std::string &element,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

    public:
        void unbindPdmComponentWithRBDBlock(
                std::size_t initiatingService,
                const WiSemanticsPairQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void unbindRbdSchemaWithSubRbd(
                std::size_t initiatingService,
                const WiSemanticsPairQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void ungroupSubRbd(
                std::size_t initiatingService,
                WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteRBDs(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteRBD(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> addFunctionalUnits(
                std::size_t initiatingService,
                const WiNewFunctionalUnits &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void deleteFunctionalUnits(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void appendFunctionalUnitsToElements(
                std::size_t initiatingService,
                const WiAppendFunctionalUnitsToElements &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void appendFunctionalUnitsToElement(
                std::size_t initiatingService,
                const std::set<std::string>& functionalUnits,
                const std::string& semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> fetchFunctionalUnitsOfPdmElement(
                std::size_t initiatingService,
                WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> fetchPdmElementsOfFunctionalUnit(
                std::size_t initiatingService,
                WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> copyElementsInternal(
                std::size_t initiatingService,
                const WiAddElementsToProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void prepareElementForDeletionToBin(
                std::size_t initiatingService,
                const WiPdmRawNodeEntity &node,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
        std::optional<WiSemanticResult> deleteElementToBin(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> deleteElementToBinInternal(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> deleteWithDescendants(
                std::size_t initiatingService,
                const SemanticsHierarchyNode& desc,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> restoreElementsFromBin(
            std::size_t initiatingService,
            const WiRestoreElementsQuery &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> copyElements(
                std::size_t initiatingService,
                const WiAddElementsToProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // all types of components addition here
        std::optional<WiSemanticResult> addContainerToProject(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addContainerToProjectInternal(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addContainerCopyToProject(
                std::size_t initiatingService,
                const WiAddComponentToProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addBlankComponentToProject(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addBlankComponentToProjectInternal(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addComponentToProject(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addComponentToProjectInternal(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> addElementsToProject(
                std::size_t initiatingService,
                const WiAddComponentsWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> addElementsToProjectInternal(
                std::size_t initiatingService,
                const WiAddComponentsWithData &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addProxyComponentToProject(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addProxyComponentToProjectInternal(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addComponentCopyToProject(
                std::size_t initiatingService,
                const WiAddComponentToProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> addComponentDefinitionToProject(
                std::size_t initiatingService,
                const WiAddComponentToProject &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmComponentData> fetchComponentData(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void updateComponentData(
                std::size_t initiatingService,
                const WiUpdateComponentQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<std::vector<WiHandbookMetadata>> getHandbooksMetadata(
                std::size_t initiatingService,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void lockNode(
                std::size_t initiatingService,
                std::string semantic,
                std::int32_t role,
                bool propagate,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void unlockNode(
                std::size_t initiatingService,
                std::string semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> moveElementsInternal(
                std::size_t initiatingService,
                const WiMoveElementsQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticsResult> moveElements(
                std::size_t initiatingService,
                const WiMoveElementsQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить список проектов
        std::optional<WiPdmNodeView::Container> fetchProjectsView(
                std::size_t initiatingService,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Сервисная информация о проекте
        std::optional<WiProjectServiceInfo> fetchProjectServiceInfo(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiActorView> fetchProjectLatestUpdate(
                std::size_t initiatingService,
                WiFetchProjectLatestUpdateQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void fetchProjectLatestUpdateInternal(
                std::size_t initiatingService,
                WiActorView& _last_change,
                const std::string &semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        // Выгрузить список проектов
    private:
        std::optional<WiPaginatedView> fetchProductRNViewInternal(
                std::size_t initiatingService,
                const WiFetchRNTableElementsQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);
    public:

        std::optional<WiPaginatedView> fetchProductRNView(
                std::size_t initiatingService,
                const WiFetchRNTableElementsQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузка данных всех компонентов из таблицы расчет надежности в файл xlsx
        std::optional<WiFileResponse> exportRNComponentsToXlsx(
                std::size_t initiatingService,
                const WiExportRNComponentsQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить полное представление для проекта
        std::optional<WiPdmTreeView> fetchProjectView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Закрыть текущий проект
        void closeProjectView(
                std::size_t initiatingService,
                const WiSemanticOnlyQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить структуру элемента проекта
        std::optional<WiRawTree<WiPdmTreeEntityItemView>> fetchElementStructure(
              std::size_t initiatingService,
              const std::string &semantic,
              std::optional<std::int32_t> &language,
              const std::shared_ptr<IWiSession> &sessionPtr,
              boost::system::error_code &ec,
              const net::yield_context &yield,
              std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Получить Entity элементов проекта
        std::optional<WiPdmRawEntity::Container> fetchElementsData(
                std::size_t initiatingService,
                const WiSemanticsOnlyQuery &query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить структуру элемента проекта с использованием глубины
        std::optional<WiRawTree<WiPdmTreeEntityItemView>> fetchElementStructureUsingDepth(
            std::size_t initiatingService,
            const WiSemanticAndDepthQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить представление узла
        std::optional<WiPdmNodeView> fetchNodeView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить представление слоя
        std::optional<WiPdmNodeView::Container> fetchNodesView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmNodeView::Container> fetchProjectComponents(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить представление данных узла
        std::optional<WiPdmEntityView> fetchEntityView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmTreeView> fetchTreeRoleView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmTreeEntityView> fetchTreeEntityRoleView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmTreeEntityView> fetchTreeCompoundEntityView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmTreeWeb> fetchTreeCompoundWebView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiPdmNodeView> apply(const WiPdmRawNode &source, boost::system::error_code &ec) const noexcept(true);

    private:
        inline std::shared_ptr<WiPdmRawNode> fetchRawNode(
            const std::string &semantic,
            boost::system::error_code &ec, const net::yield_context &yield,std::shared_ptr<MethodContextInterface> ctx, bool forceUpdate=false) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            boost::ignore_unused(mctx);
            if(forceUpdate){
                WiCacheSvc.remove<WiPdmRawNode>(semantic);
                WiCacheSvc.remove<WiPdmRawNodeEntity>(semantic);
                WiCacheSvc.remove<WiPdmRawNode::Container>(semantic);
                WiCacheSvc.remove<WiPdmRawNodeEntity::Container>(semantic);
            }
            return WiCacheSvc.getAsync<WiPdmRawNode>(semantic, mctx, ec, yield);
        }
        inline std::shared_ptr<WiPdmRawNode::Container> fetchRawNodes(std::string &semantic, boost::system::error_code &ec, const net::yield_context &yield,std::shared_ptr<MethodContextInterface> ctx,bool forceUpdate=false) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            boost::ignore_unused(mctx);
            if(forceUpdate){
                WiCacheSvc.remove<WiPdmRawNode>(semantic);
                WiCacheSvc.remove<WiPdmRawNodeEntity>(semantic);
                WiCacheSvc.remove<WiPdmRawNode::Container>(semantic);
                WiCacheSvc.remove<WiPdmRawNodeEntity::Container>(semantic);
            }
            return WiCacheSvc.getAsync<WiPdmRawNode::Container>(semantic, mctx, ec, yield);
        }
        std::optional<WiPdmStatus> getStatus(std::int64_t id, boost::system::error_code &ec) const noexcept(true);

        /*!
        * @brief Формирует запрос WiUpdatePdmNodeQuery для изменения внутренних данных компонента
        * @param[in] query - исходный запрос 
        * @param[in, out] updateQuery - возвращаемый запрос содержащий нужные обновления
        * @param[in, out] node
        * @param[in, out] ec
        * @param[in] yield
        * @param[in] ctx
        */
        void updateComponentData_impl(
            std::size_t initiatingService,
            const WiUpdateComponentQuery& query,
            WiUpdatePdmNodeQuery& updateQuery, //out
            std::shared_ptr<WiPdmRawNodeEntity> node,
            std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);


        /*!
        * @brief Формирует запрос WiUpdatePdmNodeQuery для изменения роли компонента
        * @param[in] initiatingService
        * @param[in] query - исходный запрос
        * @param[in, out] updateQuery - возвращаемый запрос содержащий нужные обновления
        * @param[in, out] node
        * @param[in] sessionPtr
        * @param[in, out] ec
        * @param[in] yield
        * @param[in] ctx
        */
        void transformElement_impl(
            std::size_t initiatingService,
            const WiUpdateComponentQuery &query,
            WiUpdatePdmNodeQuery& updateQuery, //out
            std::shared_ptr<WiPdmRawNodeEntity> node,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);


        /*!
        * @brief Проверяет валидность запроса и ноды перед передачей в функции transformElement_impl и updateComponentData_impl
        * @param[in, out] query - исходный запрос
        * @param[in, out] node
        * @param[in, out] ec - если = 0, то query и node валидны
        */
        void updateComponentQueryCheck(const WiUpdateComponentQuery &query,
                                       std::shared_ptr<WiPdmRawNodeEntity> node,
                                       boost::system::error_code &ec) const noexcept(true);
        

        void BindPdmComponentWithRbdBlockInternal(
                std::size_t initiatingService,
                const std::string& pdmComponentSemantic,
                const std::string& RBDBlockSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

            // Если семантика компонента пустая - очищает переменные блока
        void fillRbdBlockVars(
                std::size_t initiatingService,
                const std::string& pdmComponentSemantic,
                const std::string& RBDBlockSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void BindRbdSchemaWithSubRbdInternal(
                std::size_t initiatingService,
                const std::string& RbdSchemaSemantic,
                const std::string& SubRbdSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void unbindPdmComponentWithRbdBlockInternal(
                std::size_t initiatingService,
                const std::string& pdmComponentSemantic,
                const std::string& RBDBlockSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void unbindRbdSchemaWithSubRbdInternal(
                std::size_t initiatingService,
                const std::string& RbdSchemaSemantic,
                const std::string& SubRbdSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);


        // Выгрузить дерево в плоскую структуру нативных узлов
        void fetchFlatRawTree(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawTreeNode::Container &container,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void fetchFlatRawTree(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawTreeNodeEntity::Container &container,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // выгрузить нативный узел
        std::shared_ptr<WiPdmRawNode> fetchRawNode(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr,bool forceupdate=false) const noexcept(true);

        std::shared_ptr<WiPdmRawNodeEntity> fetchRawNodeEntity(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить слой нативных объектов
        void fetchRawNodes(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawNode::Container &container,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void fetchRawNodesEntity(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawNodeEntity::Container &container,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Реверсивно выгрузить цепочку данных узлов
        void fetchReverseEntityTree(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawEntity::Container &container,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        // Выгрузить данные узла
        void fetchRawEntity(
                std::size_t initiatingService,
                const std::string &semantic,
                WiPdmRawEntity &entity,
                const std::shared_ptr<IWiSession>& sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::string addNewNodeInternal(
                std::size_t initiatingService,
                WiNewPdmNodeQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                const Filter &filter,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> moveNodeInternal(
                std::size_t initiatingService,
                const WiMovePdmNodeQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                const Filter &filter,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

//        template<typename Result, typename BinaryOperation>
//        Result accumulateOverShortNodes(std::string &semantic,const std::shared_ptr<IWiSession>& sessionPtr, boost::system::error_code &ec, const net::yield_context &yield, Result init, BinaryOperation op);

    private:
        void checkNode(
                std::size_t initiatingService,
                const std::string &semantic,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void rearrangePositionalIndecies(
                std::size_t initiatingService,
                const WiRearrangePostionalIndecies& query,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void assignPositionals(
                std::size_t initiatingService,
                std::vector<std::uint32_t> &pos,
                const WiPdmRawNode::Container& nodes,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::size_t depth,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void assignPositional(
                std::size_t initiatingService,
                std::vector<std::uint32_t> &pos,
                std::string& semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::size_t depth,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void checkElementDestination(
                std::size_t initiatingService,
                const WiAddComponentWithData &query,
                int role,
                std::optional<int> type,
                bool checkBin,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void checkDescendantsDestination(
                std::size_t initiatingService,
                std::shared_ptr<WiPdmRawNodeEntity>,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void copyAllDescendantsTo(
                std::size_t initiatingService,
                const std::string &source,
                const std::string &destination,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        int getPdmComponentRoleOfMdmComponent(
                std::size_t initiatingService,
                const std::string& semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        bool updateElementFunctionalUnits(
                std::size_t initiatingService,
                const std::string& prod,
                WiPdmElementData &data,
                const WiUpdateFunctionalUnitsList fus,
                std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true);

        bool setFunctionalUnits(
                std::size_t initiatingService,
                WiPdmElementData &data,
                const std::set<std::string>& functionalUnits,
                std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        bool appendFunctionalUnits(
                std::size_t initiatingService,
                WiPdmElementData &data,
                const std::set<std::string>& functionalUnits,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        bool eraseFunctionalUnits(
                std::size_t initiatingService,
                WiPdmElementData &data,
                const std::set<std::string>& functionalUnits,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void eraseFunctionalUnitFromElements_internal(
                std::size_t initiatingService,
                const std::string& fu_semantic,
                const std::set<std::string>& elem_semantics,
                std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void appendPdmElementToFunctionalUnits_internal(
                std::size_t initiatingService,
                const std::string &elem_semantic,
                const std::set<std::string>& functionalUnits,
                std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        void erasePdmElementFromFunctionalUnits_internal(
                std::size_t initiatingService,
                const std::string &elem_semantic,
                const std::set<std::string>& functionalUnits,
                std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        WiUpdatePdmNodeQuery appendPdmElementToFunctionalUnit_internal(
                std::size_t initiatingService,
                const std::string &elem_semantic,
                const std::string& functionalUnit,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        WiUpdatePdmNodeQuery erasePdmElementFromFunctionalUnit_internal(
                std::size_t initiatingService,
                const std::string &elem_semantic,
                const std::string& functionalUnit,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::set<std::string> getFunctionalUnitRefs_internal(
                std::size_t initiatingService,
                const std::string &fu_semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::set<std::string> getFunctionalUnitsOfElement_internal(
                std::size_t initiatingService,
                const std::string &elem_semantic,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code& ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        WiPdmRawNode nodeNearestAncestor(
                std::size_t initiatingService,
                const std::string &semantic,
                int role,
                int depth,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        WiPdmRawNode nodeNearestDescendant(
                std::size_t initiatingService,
                const std::string &semantic,
                int role,
                int depth,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        std::optional<WiSemanticResult> createComponentRefInternal(
                std::size_t initiatingService,
                const std::string &semantic,
                const std::string &source,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true);

        template <typename NodeView>
        inline void fetchNodesView(
                std::size_t initiatingService,
                typename NodeView::Container &container,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();

            boost::ignore_unused(sessionPtr);
            boost::ignore_unused(initiatingService);

            WiPdmRawNode::Container rawNodeContainer;
            fetchRawNodes(initiatingService,semantic,  rawNodeContainer,sessionPtr, ec, yield,mctx);
            if (ec) {
                return;
            }

            auto statusResolver = [this](auto &&id, auto &&ec) {
                return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
            };

            auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
                return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
            };

            using StatusResolverType = decltype(statusResolver);
            using FioResolverType = decltype(fioResolver);

            ::apply<WiPdmRawNode, NodeView, WiPdmStatus, StatusResolverType, FioResolverType>(
                    rawNodeContainer,
                    container,
                    language,
                    ec,
                    std::forward<StatusResolverType>(statusResolver),
                    std::forward<FioResolverType>(fioResolver));
        }

        template <typename NodeView>
        inline void fetchNodesView(
                std::size_t initiatingService,
                typename NodeView::Container &container,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            boost::ignore_unused(initiatingService,sessionPtr);
            GUARD_PDM_METHOD_PUB();

            checkNode(initiatingService, semantic, role, sessionPtr,  ec, yield,mctx);
            if (ec) {
                return;
            }
            fetchNodesView<NodeView>(container, semantic, language, sessionPtr, ec, yield,mctx);
        }

        template <typename NodeView>
        inline std::optional<typename NodeView::Container> fetchNodesView(
               std::size_t initiatingService,
               const std::string &semantic,
               std::optional<std::int32_t> &language,
               const std::shared_ptr<IWiSession> &sessionPtr,
               boost::system::error_code &ec,
               const net::yield_context &yield,
               std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            typename NodeView::Container container;
            fetchNodesView<NodeView>(initiatingService, container, semantic, language, sessionPtr, ec, yield, mctx);
            if (ec) {
                return std::nullopt;
            }
            return std::make_optional<typename NodeView::Container>(std::move(container));
        }

        template <typename NodeView>
        inline std::optional<typename NodeView::Container> fetchNodesView(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            typename NodeView::Container container;
            fetchNodesView<NodeView>(initiatingService, container, semantic, language, role, sessionPtr, ec, yield,mctx);
            if (ec) {
                return std::nullopt;
            }
            return std::make_optional<NodeView::Container>(std::move(container));
        }

        template <typename TreeViewItem>
        inline void fetchFlatTreeNodeView(
                std::size_t initiatingService,
                typename TreeViewItem::WiNodeType::Container &container,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            typename TreeViewItem::WiRawNodeType::Container rawTreeNodeEntityContainer;
            fetchFlatRawTree(initiatingService, semantic, rawTreeNodeEntityContainer, sessionPtr, ec, yield,mctx);

            if (ec) {
                return;
            } else {
                auto statusResolver = [this](auto &&id, auto &&ec) {
                    return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
                };

                auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
                    return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
                };

                using StatusResolverType = decltype(statusResolver);
                using FioResolverType = decltype(fioResolver);

                wi::basic_services::apply<typename TreeViewItem::WiRawNodeType, typename TreeViewItem::WiNodeType, WiPdmStatus, StatusResolverType, FioResolverType>(
                    rawTreeNodeEntityContainer,
                    container,
                    language,
                    ec,
                    std::forward<StatusResolverType>(statusResolver),
                    std::forward<FioResolverType>(fioResolver));
            }
        }

        template <typename TreeViewItem>
        inline void fetchFlatTreeNodeView(
                std::size_t initiatingService,
                typename TreeViewItem::WiNodeType::Container &container,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            boost::ignore_unused(initiatingService,sessionPtr);
            GUARD_PDM_METHOD_PUB();
            checkNode(initiatingService, semantic, role,sessionPtr,  ec, yield,mctx);
            if (ec) {
                return;
            }
            fetchFlatTreeNodeView<TreeViewItem>(initiatingService, container, semantic, language, sessionPtr, ec, yield,mctx);
        }

        template <typename TreeViewItem>
        inline std::optional<WiRawTree<TreeViewItem>> fetchTree(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();

            typename TreeViewItem::WiNodeType::Container container;
            fetchFlatTreeNodeView<TreeViewItem>(initiatingService, container, semantic, language, sessionPtr, ec, yield,mctx);

            if (ec) {
                return std::nullopt;
            }

            WiRawTree<TreeViewItem> treeView(container);

            if (!treeView.has_data()) {
                ec = make_error_code(node_not_found);
                return std::nullopt;
            }
            return std::make_optional<WiRawTree<TreeViewItem>>(std::move(treeView));
        }

        template <typename TreeViewItem>
        inline std::optional<WiRawTree<TreeViewItem>> fetchTree(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                std::int32_t role,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            checkNode(initiatingService, semantic, role, sessionPtr,  ec, yield,mctx);
            if (ec) {
                return std::nullopt;
            }
            return fetchTree<TreeViewItem>(initiatingService, semantic, language, sessionPtr, ec, yield,mctx);
        }

        template <typename TreeViewItem>
        inline std::optional<WiRawTree<TreeViewItem>> fetchTreeCompound(
                std::size_t initiatingService,
                const std::string &semantic,
                std::optional<std::int32_t> &language,
                const std::shared_ptr<IWiSession> &sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx = nullptr) const noexcept(true) {
            GUARD_PDM_METHOD_PUB();
            WiPdmRawNode::Container container;
            fetchRawNodes(initiatingService,semantic, container, sessionPtr, ec, yield,mctx);
            if (ec) {
                return std::nullopt;
            }
            else {
                if (container.empty()) {
                    ec = make_error_code(node_not_found);
                    return std::nullopt;
                }
            }

            auto predicate = [](const auto &item) {
                return item.role == PdmRoles::ProjectComposition;
            };

            auto count = std::count_if(container.begin(), container.end(), predicate);
            if (count > 1) {
                ec = make_error_code(too_many_nodes_this_role);
                return std::nullopt;
            }

            if (count < 1) {
                ec = make_error_code(node_not_found);
                return std::nullopt;
            }

            auto it = std::find_if(container.begin(), container.end(), predicate);

            return fetchTree<TreeViewItem>(initiatingService, it->semantic, language, sessionPtr, ec, yield,mctx);
        }

        void updateProjectsProjectIdBySystem(
                std::size_t initiatingService,
                uint64_t index,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const;

    private:
        std::unique_ptr<std::mt19937_64> m_mt;
        WiPdmStatus::Map m_statuses;
        std::int32_t m_defaultLanguage;
        mutable IWiPlatform::PlatformEventNumeratorPtr m_eventNumerator;
        std::shared_ptr<net::io_context::strand> container_update_strand;
        std::shared_ptr<net::io_context::strand> product_update_strand;
        std::shared_ptr<net::io_context::strand> rbd_update_strand;
    };
}

#endif
