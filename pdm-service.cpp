#include <optional>
#include <algorithm>
#include <chrono>
#include <boost/bind.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/algorithm/string.hpp>
#include <wi-events.hpp>
#include <data-access-router.hpp>
#include <handbooks/handbooks-manager-service.hpp>
#include <internal/event-bus-service-helper.hpp>
#include <core-all.hpp>
#include <pdm-error.hpp>
#include "internal/mdm-roles.hpp"
#include "pdm-roles.hpp"
#include <wi-dto-utils.hpp>
#include <internal/users-service.hpp>
#include <wi-dto-applies.hpp>
#include <string-helper.hpp>
#include "internal/mdm-service.hpp"
#include <iwi-platform.hpp>
#include <utils.hpp>
#include "stereotype/stereotype-helpers.hpp"
#include <reflection/json-reflector.hpp>
#include <cctype>
#include <iostream>
#include <cstring>
#include <boost/math/quadrature/gauss_kronrod.hpp>
#include <boost/math/quadrature/exp_sinh.hpp>

#include <thread>
#include <cmath>

#include "pdm-service.hpp"

#include <export/include/export_sort_order.h>

#include "translate-text-helper.hpp"
#include "wi-plain-cache.h"

#include "export/include/xlnt_xlsx_writer.hpp"
#include "export/include/component_grouper.h"
#include "handbooks/mil217F-PC/models/microcircuits.hpp"


using namespace wi::core::bus::event::events;
using namespace wi::core::data_access;
using namespace wi::basic_services;
using namespace wi::basic_services::pdm::error;
using namespace wi::basic_services::users::internal;
using namespace wi::core::platform;
using namespace wi::core::utils;
using namespace wi::core;
using namespace wi::core::json::reflection;

using namespace Number_literals;

namespace wi::basic_services::pdm::internal {

using gauss_kronrod = boost::math::quadrature::gauss_kronrod<Number,150>; // best so far
// gauss_kronrod::weights();gauss_kronrod::abscissa();
// those 2 funcs can be used to preload a weight tables - for 61 they are hardocded
// but if you are changing it - please reference boost docs

class RbdModel{
    struct _visitor {
        const Number &timespan;
        std::optional<Number> result;
        _visitor(const Number& ts)
            :timespan(ts) {}
        void operator()(const details::Node& node) {
            if(node.fr == 0_Nr)
            {
                result = 1_Nr;
            }
            else {
                result = exp(-node.fr * timespan);
            }
        }
        void operator()(const details::LinearModelPtr& linear) {
            if(!linear) return;
            for(const auto& part:linear->chain) {
                auto vis = _visitor(timespan);
                std::visit(vis,part);
                if(!vis.result.has_value()) {
                    continue;
                }
                if(result.has_value()) {
                    result.value() *= vis.result.value();
                }
                else {
                    result = vis.result;
                }
            }
        }
        void operator()(const details::ReservedModelPtr& reserved) {
            if(!reserved) return;
            for(const auto& part:reserved->chains) {
                auto vis = _visitor(timespan);
                std::visit(vis,part);
                if(!vis.result.has_value()) continue;
                if(result.has_value()) {
                    result.value() *= (1_Nr - vis.result.value());
                }
                else {
                    result = 1_Nr - vis.result.value();
                }
            }
            if(result.has_value()) {
                result = 1_Nr - result.value();
            }
        }
    };
public:
    RbdModel(details::RBDPartModel &&model):m(std::move(model)){}
    std::optional<Number> calculate(Number timespan){
//        WI_LOG_INFO() << "current model" << toJson(m).dump() ;
        auto vis = _visitor(timespan);
        std::visit(vis,m);
        return vis.result;
    }
private:
    const details::RBDPartModel m;
};

class PdmMethodGuard;
class PdmMethodContext:public wi::core::MethodContextInterface, public std::enable_shared_from_this<PdmMethodContext>{
    private:
        friend class PdmMethodGuard;
        std::shared_ptr<wi::core::MethodContextInterface> underlying;
        const std::size_t m_initiatingService;
        const std::shared_ptr<IWiSession> sessionPtr;
        struct{
            // отсортированы лексикографически >, что бы идти от листьев к корню дерева ЛСИ.
            std::set<std::string,std::greater<std::string>> restored_elements;
            std::set<std::string,std::greater<std::string>> elements;
            std::set<std::string> products;
            std::set<std::string> schemas;
            std::set<std::string> schemas_flags;
        } triggers;
        virtual void beforeCommit(boost::system::error_code &ec, const net::yield_context &yield) override{
            boost::ignore_unused(ec,yield);
            //elements restored
            for(const auto &element:triggers.restored_elements){
                PdmSvc.recalculateRestoredElement(m_initiatingService,element,sessionPtr,ec,yield,shared_from_this());
            }
            //elements
            for(const auto &element:triggers.elements){
                boost::system::error_code tec;
                std::optional<std::int32_t> lang;
                auto node = PdmSvc.fetchNodeView(m_initiatingService,element,lang,sessionPtr,tec,yield,shared_from_this());
                if(!node || tec) continue;
                switch(node->role){
                    case PdmRoles::Container:
                        PdmSvc.recalculateContainer(m_initiatingService,element,sessionPtr,ec,yield,shared_from_this());
                        break;
                    case PdmRoles::Product:
                        PdmSvc.recalculateProduct(m_initiatingService,element,sessionPtr,ec,yield,shared_from_this());
                        break;
                }
            }
            triggers.elements.clear();
            // products
            for(const auto &product:triggers.products){
                boost::system::error_code tec;
                std::optional<std::int32_t> lang;
                auto node = PdmSvc.fetchNodeView(m_initiatingService,product,lang,sessionPtr,tec,yield,shared_from_this());
                if(!node || tec) continue;
                if(node->role != PdmRoles::Product) continue;
                PdmSvc.recalculateProductFull(m_initiatingService,product,sessionPtr,ec,yield,shared_from_this());

            }
            triggers.products.clear();
            //schemas
            for(const auto &schema:triggers.schemas){
                boost::system::error_code tec;
                std::optional<std::int32_t> lang;
                auto node = PdmSvc.fetchNodeView(m_initiatingService,schema,lang,sessionPtr,tec,yield,shared_from_this());
                if(!node || tec) continue;
                if(node->role != PdmRoles::RbdSchema) continue;
                PdmSvc.recalculateRbd(m_initiatingService,schema,sessionPtr,ec,yield,shared_from_this());
            }
            triggers.schemas.clear();
            // schema flags
            for(const auto &schema:triggers.schemas_flags){
                boost::system::error_code tec;
                std::optional<std::int32_t> lang;
                auto node = PdmSvc.fetchNodeView(m_initiatingService,schema,lang,sessionPtr,tec,yield,shared_from_this());
                if(!node || tec) continue;
                if(node->role != PdmRoles::RbdSchema) continue;
                PdmSvc.provisionRbdFlags(m_initiatingService,schema,sessionPtr,ec,yield,shared_from_this());
            }
            triggers.schemas_flags.clear();
        };

    public:
        virtual operator wi::lib::database::DateAccessTransactionPtr() override{
            return underlying->operator wi::lib::database::DateAccessTransactionPtr();
        };
        virtual void commit(boost::system::error_code &ec, const net::yield_context &yield) override {
            beforeCommit(ec,yield);
            underlying->commit(ec, yield);
        }
        virtual void cancel(boost::system::error_code &ec, const net::yield_context &yield) override{
            underlying->cancel(ec, yield);
        }
        PdmMethodContext(lib::database::DateAccessTransactionPtr ptr,const std::size_t initiatingService, const std::shared_ptr<IWiSession> sessionPtr):underlying(std::make_shared<wi::core::MethodContext>(ptr)),m_initiatingService(initiatingService),sessionPtr(sessionPtr){}
        PdmMethodContext(std::shared_ptr<MethodContextInterface> ctx,const std::size_t initiatingService, const std::shared_ptr<IWiSession> sessionPtr):underlying(ctx),m_initiatingService(initiatingService),sessionPtr(sessionPtr){};

        virtual void fire(IWiPlatform::PdmAddNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::PdmUpdateNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::PdmDeleteNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::MdmAddNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::MdmUpdateNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::MdmDeleteNodeEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::UsersAddEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual void fire(IWiPlatform::UsersUpdateEvent && event) override {
            if(underlying){
                underlying->fire(std::forward<decltype(event)>(event));
            }
        }
        virtual ~PdmMethodContext()=default;

        void addRestoredElementTrigger(const std::string& element){
            triggers.restored_elements.insert(element);
        }
        void addElementTrigger(const std::string& element){
            triggers.elements.insert(element);
        }
        void addProductTrigger(const std::string& product){
            triggers.products.insert(product);
        }
        void addSchemaTrigger(const std::string& schema){
            triggers.schemas.insert(schema);
        }
        void addSchemaFlagsTrigger(const std::string& schema){
            triggers.schemas_flags.insert(schema);
        }

        boost::system::error_code &ec() {
        }

        std::size_t initiatingService() {
            return m_initiatingService;
        }
    };
    class PdmMethodGuard : public wi::core::MethodGuard{
    private:
        bool pmc_owner = false;
        std::shared_ptr<PdmMethodContext> _to_pmc(){
            return std::dynamic_pointer_cast<PdmMethodContext>(ctx);
        }
    public:
        PdmMethodGuard(std::shared_ptr<MethodContextInterface> &_ctx, boost::system::error_code&_ec, const net::yield_context &_yield, std::chrono::milliseconds timeout,const std::size_t initiatingService,const std::shared_ptr<IWiSession> sessionPtr) : wi::core::MethodGuard(_ctx,_ec,_yield,timeout){
            if(!_to_pmc()){
                pmc_owner = true;
                ctx = std::make_shared<PdmMethodContext>(_ctx,initiatingService,sessionPtr);
            }
        }
        virtual ~PdmMethodGuard(){
            if(pmc_owner && !isOwner()){
                if(auto pmc = _to_pmc()){
                    pmc->beforeCommit(ec,yield);
                }
            }
        };

        void addRestoredElementTrigger(const std::string& element){
            auto pmc = _to_pmc();
            if(pmc){
                pmc->addRestoredElementTrigger(element);
            }
        }
        void addElementTrigger(const std::string& element){
            auto pmc = _to_pmc();
            if(pmc){
                pmc->addElementTrigger(element);
            }
        }
        void addProductTrigger(const std::string& element){
            auto pmc = _to_pmc();
            if(pmc){
                pmc->addProductTrigger(element);
            }
        }
        void addSchemaTrigger(const std::string& element){
            auto pmc = _to_pmc();
            if(pmc){
                pmc->addSchemaTrigger(element);
            }
        }
        void addSchemaFlagsTrigger(const std::string& element){
            auto pmc = _to_pmc();
            if(pmc){
                pmc->addSchemaFlagsTrigger(element);
            }
        }
    };

    #define GUARD_PDM_METHOD() PdmMethodGuard mctx(ctx,ec,yield,std::chrono::milliseconds(WI_CONFIGURATION().read_settings<size_t>(server_method_timeout)),initiatingService,sessionPtr)
    void fillRBDVars(WiPdmElementVariables &vars) {
        if(vars.reliability.has_value()){
            vars.failure_probability = 1_Nr - vars.reliability.value();
        }
        if(vars.MTBF.has_value()) {
            // Если средняя наработка на отказ бесконечна, то интенсивность отказов равна нулю
            if(isinf(vars.MTBF.value())) {
                vars.failure_rate = 0_Nr;
                // Если средняя наработка на отказ ноль, то интенсивность отказов бесконечна
            } else if(0_Nr == vars.MTBF.value()) {
                vars.failure_rate = std::numeric_limits<Number>::infinity();
            } else {
                vars.failure_rate = 1_Nr / vars.MTBF.value();
            }
        }
    }
    void fillAllVars(WiPdmElementVariables &vars, std::optional<long double> timespan, boost::system::error_code &ec){
        boost::ignore_unused(ec);
        if (vars.set_reliability.has_value() && vars.set_reliability_time.has_value()){
            auto sr = vars.set_reliability.value();
            auto st = vars.set_reliability_time.value();
            if(sr>= 1_Nr or sr <= 0_Nr or st <= 0_Nr) {
                ec = make_error_code(error::invalid_input_data);
                return;
            }
            vars.failure_rate = log(1_Nr / vars.set_reliability.value()) / vars.set_reliability_time.value();
            vars.MTBF = 1_Nr /vars.failure_rate.value();
            if(timespan.has_value()) {
                vars.reliability = pow(sr,(Number(timespan.value())/st));
                vars.failure_probability = 1_Nr - vars.reliability.value();
            }
        }
        else if(vars.failure_rate.has_value()){
            vars.MTBF = 1_Nr /vars.failure_rate.value();
            if(timespan.has_value()) {
                vars.reliability = exp(-vars.failure_rate.value() * timespan.value());
                vars.failure_probability = 1_Nr - vars.reliability.value();
            }
        }else if(vars.MTBF.has_value()){
            vars.failure_rate = 1_Nr / vars.MTBF.value();
            if(timespan.has_value()) {
                vars.reliability = exp(-vars.failure_rate.value() * timespan.value());
                vars.failure_probability = 1_Nr - vars.reliability.value();
            }
        }else if(vars.reliability.has_value()){
            if(timespan.has_value()) {
                vars.failure_rate = -log(vars.reliability.value())/timespan.value();
                vars.MTBF = 1_Nr/vars.failure_rate.value();
                vars.failure_probability = 1_Nr - vars.reliability.value();
            }
        }else if(vars.failure_probability.has_value()){
            if(timespan.has_value()) {
                vars.reliability = 1_Nr - vars.failure_probability.value();
                vars.failure_rate = -log(vars.reliability.value())/timespan.value();
                vars.MTBF = 1_Nr/vars.failure_rate.value();
            }
        }

        if(vars.MTBF != vars.MTBF )
        {
            vars.MTBF = std::nullopt;
        }
        if(vars.failure_probability != vars.failure_probability )
        {
            vars.failure_probability = std::nullopt;
        }
        if(vars.failure_rate != vars.failure_rate )
        {
            vars.failure_rate = std::nullopt;
        }
        if(vars.reliability != vars.reliability )
        {
            vars.reliability = std::nullopt;
        }
    }

    WiPdmElementVariables calculateAll(std::optional<Number> failure_rate, std::optional<long double> timespan){
        WiPdmElementVariables vars;
        vars.failure_rate = failure_rate;
        boost::system::error_code ec;
        fillAllVars(vars,timespan,ec);
        return vars;
    }

    std::pair<std::string,std::int32_t> positional_parse_data(std::string new_data){
        new_data.erase(std::remove_if(new_data.begin(),new_data.end(),isspace),new_data.end());
        auto rit1 = std::find_if_not(new_data.rbegin(),new_data.rend(),isdigit);
        std::string letter_part_new_data(new_data.begin(),rit1.base());
        std::string st_digital_part_new_data(rit1.base(),new_data.end());
        std::int32_t digital_part_new_data = atoi(st_digital_part_new_data.c_str());
        return std::make_pair(letter_part_new_data,digital_part_new_data);
    }
    void positional_remove_old_from_prod_data(std::string &letter_part_old_data, std::int32_t digital_part_old_data ,WiPdmElementData &prod_data,boost::system::error_code &ec,bool &marking_bool) noexcept(true){
        // т.к. кеш может быть невалидным - просто пропускаем чистку.
        if (prod_data.positional_indexies_cache[letter_part_old_data].find(digital_part_old_data)!=prod_data.positional_indexies_cache[letter_part_old_data].end() and digital_part_old_data!=0)
        {
            prod_data.positional_indexies_cache[letter_part_old_data].erase(digital_part_old_data); //чистка
            marking_bool = true;
        }
    }
    std::string positional_index_cache_conversion (std::string new_data,std::string old_data ,WiPdmElementData &prod_data,boost::system::error_code &ec,bool &marking_bool) noexcept(true){

        auto new_data_parsed = positional_parse_data(new_data);
        auto& letter_part_new_data = new_data_parsed.first;
        auto& digital_part_new_data = new_data_parsed.second;

        auto old_data_parsed = positional_parse_data(old_data);
        auto& letter_part_old_data = old_data_parsed.first;
        auto& digital_part_old_data = old_data_parsed.second;
        //конец, разделения маркировки
        if(letter_part_old_data == letter_part_new_data && digital_part_old_data == digital_part_new_data ){
            return "";
        }

        if (prod_data.positional_indexies_cache.find(letter_part_new_data) != prod_data.positional_indexies_cache.end() && letter_part_old_data.empty()) //проверка наличия - ключа
        {
            positional_remove_old_from_prod_data(letter_part_old_data,digital_part_old_data,prod_data,ec,marking_bool);

            if (digital_part_new_data==0)
            {
                std::int32_t min = 1;
                bool proc = true;
                while (proc)
                {
                    if(prod_data.positional_indexies_cache[letter_part_new_data].find(min)!=prod_data.positional_indexies_cache[letter_part_new_data].end()) //ищем минимальное - свободное место
                    {
                        min+=1;
                    }
                    else
                    {
                        marking_bool = true;
                        prod_data.positional_indexies_cache[letter_part_new_data].insert(min);
                        new_data = letter_part_new_data+std::to_string(min);
                        return new_data;
                    }
                }
            }
            marking_bool = true;
            prod_data.positional_indexies_cache[letter_part_new_data].insert(digital_part_new_data);
            new_data = letter_part_new_data+std::to_string(digital_part_new_data);
            return new_data;
        }
        else
        {
            if(prod_data.positional_indexies_cache[letter_part_old_data].empty()) {
                positional_remove_old_from_prod_data(letter_part_old_data,digital_part_old_data,prod_data,ec,marking_bool);
                if (digital_part_new_data == 0) {
                    marking_bool = true;
                    prod_data.positional_indexies_cache[letter_part_new_data] = {1};
                    new_data = letter_part_new_data + std::to_string(1);
                    return new_data;
                }
                marking_bool = true;
                prod_data.positional_indexies_cache[letter_part_new_data] = {digital_part_new_data};
                new_data = letter_part_new_data + std::to_string(digital_part_new_data);
                return new_data;
            }else {
                positional_remove_old_from_prod_data(letter_part_old_data,digital_part_old_data,prod_data,ec,marking_bool);
                marking_bool = true;
                prod_data.positional_indexies_cache[letter_part_new_data].insert(digital_part_new_data);
                if (digital_part_new_data==0)
                {
                    new_data = letter_part_new_data;
                }
                else
                {
                    new_data = letter_part_new_data + std::to_string(digital_part_new_data);
                }
                return new_data;
            }
        }
        //
        marking_bool = false;
        return old_data;
    }

    void splitSemantic(std::vector<std::string>& semantics, const std::string &semantic){
        constexpr static const char* semanticSplitter = "::";
        boost::iter_split(semantics, semantic, boost::first_finder(semanticSplitter));
    }

    std::string parentSemantic(std::string semantic, boost::system::error_code&ec){
        std::vector<std::string> semantics;
        splitSemantic(semantics,semantic);
        if (!semantics.empty()) {
            semantics.erase(semantics.end() - 1);
            if (!semantics.empty()) {
                std::string temp_semantic;
                buildSemantic(semantics, temp_semantic);
                return temp_semantic;
            }
        }
        ec = make_error_code(error::node_not_found);
        return semantic;
    }

    int semanticDepth(const std::string &parent, const std::string &descendant) {
        std::vector<std::string> par,des;
        splitSemantic(par, parent);
        splitSemantic(des, descendant);
        if(des.size() < par.size()) return -1;
        for(auto d=des.begin(), p=par.begin(); d!=des.end() and p!=par.end(); ++d, ++p){
            if(*d!=*p) return -1;
        }
        return des.size() - par.size();
    }

    bool unwrapPositional(std::vector<std::uint32_t>&target, std::string &positional){
        constexpr static const char* positionalSplitter = ".";
        std::vector<std::string> pos;
        boost::iter_split(pos, positional, boost::first_finder(positionalSplitter));

        target.clear();
        if(pos.size() > 1){
            for(const auto&d:pos){
                try{
                    target.push_back(boost::lexical_cast<std::uint32_t>(d));
                }catch(...){
                    return false;
                }
            }
            return true;
        }else{
            if(positional.empty()){
                return true;
            }else{
                try{
                    target.push_back(boost::lexical_cast<std::uint32_t>(positional));
                    return true;
                }catch(...){
                    return false;
                }
            }
        }
    }

    void buildPositional(std::vector<std::uint32_t>&values, std::string &target, boost::system::error_code&ec){

        if (values.size() == 1)
            try {
                target = boost::lexical_cast<std::string>(*begin(values));
            }catch(...){
                ec = make_error_code(error::cant_assign_positional_index);
                return;
            }
        else {
            bool f = true;
            for(auto &item: values){
                if (!f) {
                    target.append(".");
                }
                else {
                    f = false;
                }
                try {
                    target.append(boost::lexical_cast<std::string>(item));
                }catch(...){
                    ec = make_error_code(error::cant_assign_positional_index);
                    return;
                }
            }
        }
    }

    struct DefaultPositionalComparer{
    private:
        const std::vector<std::uint32_t> &pos;
    public:
        DefaultPositionalComparer(const std::vector<std::uint32_t> &pos):pos(pos){}
        template<typename NodeType>
        bool operator()(const NodeType& left, const NodeType& right){
            bool lv, rv;

            lv = left.extension.has_value();
            rv = right.extension.has_value();
            // left is less only if its valid while the other is not, if they are both invalid - incomparable
            if(!lv || !rv) return lv > rv;

            std::vector<uint32_t> lpos, rpos;
            auto ext = fromJson<WiPdmElementExtension>(left.extension.value());
            lv = unwrapPositional(lpos, ext.position);
            ext = fromJson<WiPdmElementExtension>(right.extension.value());
            rv = unwrapPositional(rpos, ext.position);
            // left is less only if its valid while the other is not, if they are both invalid - incomparable
            if(!lv || !rv) return lv > rv;

            lv = lpos.size() == pos.size() + 1;
            if(lv){
                auto it = pos.begin(); auto lit = lpos.begin();
                for(;it < pos.end(); ++it, ++lit){
                    if(*it != *lit){
                        lv = false;
                        break;
                    }
                }
            }
            rv = rpos.size() == pos.size() + 1;
            if(rv){
                auto it = pos.begin(); auto rit = rpos.begin();
                for(;it < pos.end(); ++it, ++rit){
                    if(*it != *rit){
                        rv = false;
                        break;
                    }
                }
            }
            if(!lv && !rv){
                // if both not valid, compare lexically
                return lexcompare(lpos, rpos) < 0;
            }else if(!lv || !rv){
                // if only one of them not valid, then the valid one is first
                return lv > rv;
            }else{
                // if both valid, compare only last digits
                return lpos.back() < rpos.back();
            }
        }
        static std::int32_t lexcompare(const std::vector<std::uint32_t>& lpos, const std::vector<std::uint32_t> &rpos){
            auto rit = rpos.begin(); auto lit = lpos.begin();
            for(; rit < rpos.end() && lit < lpos.end(); ++rit, ++lit){
                if(*lit < *rit){
                    return -1;
                }else if(*lit > *rit){
                    return 1;
                }
            }
            if(lpos.size() < rpos.size()) return -1;
            else if(lpos.size() > rpos.size()) return 1;
            return 0;
        }
    };

    PdmService::PdmService()
            : m_defaultLanguage(WI_CONFIGURATION().read_settings<std::int32_t>(server_language)) { }

    /*
    * public section
    */

    void PdmService::init(IWiPlatform::PlatformEventNumeratorPtr numerator, net::io_context& ios, boost::system::error_code &ec, const net::yield_context &yield) {
        // those 2 funcs can be used to preload a weight tables - for 61 they are hardcoded
        // but if you are changing it - please reference boost docs
         gauss_kronrod::weights();gauss_kronrod::abscissa();

        container_update_strand     =std::make_shared<net::io_context::strand>(ios);
        product_update_strand       =std::make_shared<net::io_context::strand>(ios);
        rbd_update_strand           =std::make_shared<net::io_context::strand>(ios);

        m_eventNumerator = IWiPlatform::PlatformEventNumeratorPtr(numerator);
        std::shared_ptr<MethodContextInterface> ctx = nullptr;
        std::size_t initiatingService = 0;
        std::shared_ptr<IWiSession> sessionPtr;
        GUARD_PDM_METHOD();
        m_mt = std::make_unique<std::mt19937_64>(time(nullptr));
        WiPdmStatus::Container pdmStatuses;
        DataAccessConst().fetchPdmStatuses(pdmStatuses, mctx, ec, yield);
        if (ec) return;
        toMap<WiPdmStatus>(pdmStatuses, m_statuses);

        PlainCache.load(mctx, ec, yield, m_defaultLanguage);
    }

    void PdmService::recalculateProductFullElement(
            std::int64_t initiatingService,
            const WiPdmRawNodeEntity &element,
            const std::optional<long double> timespan,
            bool reset,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        if (!element.entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }
        if (!element.entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }
        WiPdmElementData data = fromJson<WiPdmElementData>(element.entity->data.value());
        if(reset && (element.role == PdmRoles::Container || element.role ==PdmRoles::Product) ) {
            WiPdmElementVariables vars;
            data.variables = vars;
        }else if(data.variables.has_value()) {
            data.variables->reliability = std::nullopt;
            data.variables->failure_probability = std::nullopt;
            fillAllVars(data.variables.value(),timespan,ec);
        }
        WiUpdatePdmNodeQuery q(element);
        q.data = toJson(data);
        q.updateData = true;
        updateNode(initiatingService,q,sessionPtr,Filter::filterOn,ec, yield,mctx);
    }

    void PdmService::recalculateProductFullLayer(
            std::int64_t initiatingService,
            const WiPdmRawNodeEntity &element,
            const std::optional<long double> timespan,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        switch(element.role){
            case PdmRoles::Container:{
                WiPdmRawNodeEntity::Container nodes;
                fetchRawNodesEntity(initiatingService,element.semantic,nodes,sessionPtr,ec,yield,mctx);
                if(ec) return;
                bool reset = false;
                if(nodes.empty()){
                    reset = true;
                } else for(const auto &node:nodes){
                    recalculateProductFullLayer(initiatingService,node,timespan,sessionPtr,ec,yield,mctx);
                    if(ec) return;
                }
                recalculateProductFullElement(initiatingService,element,timespan,reset,sessionPtr,ec,yield,mctx);
                if(ec) return;
            }
                break;
            case PdmRoles::ElectricComponent:
            case PdmRoles::ProxyComponent: {
                recalculateProductFullElement(initiatingService,element,timespan,false,sessionPtr,ec,yield,mctx);
            }
                break;
        }

    }
    void PdmService::recalculateRestoredElement(
        std::int64_t initiatingService,
        const std::string &product,
        const std::shared_ptr<IWiSession> sessionPtr,
        boost::system::error_code& ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto element = fetchRawNodeEntity(initiatingService,product,sessionPtr,ec,yield,mctx);
        if(ec) return;

        auto prod_node = nodeNearestAncestor(initiatingService,element->semantic,PdmRoles::Product,1,sessionPtr,ec,yield,mctx);
        if(ec) return;
        auto prod = fetchRawNodeEntity(initiatingService,prod_node.semantic,sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(!prod->entity.has_value()) return;
        if(!prod->entity->data.has_value()) return;
        std::optional<long double> timespan;
        {
            WiPdmElementData data = fromJson<WiPdmElementData>(prod->entity->data.value());
            if(data.ster.has_value()){
                auto it = data.ster->data.find("expected_life_time");
                if(it != data.ster->data.end()){
                    auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                    if(gen_val!=nullptr){
                        auto val = std::get_if<std::optional<long double>>(gen_val);
                        if(val!=nullptr){
                            timespan = *val;
                        }
                    }
                }
            }
        }

        WiPdmRawNodeEntity::Container nodes;
        fetchRawNodesEntity(initiatingService,element->semantic,nodes,sessionPtr,ec,yield,mctx);
        if(ec)return;
        for(const auto&node:nodes){
            recalculateProductFullLayer(initiatingService,node,timespan,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        recalculateProductFullElement(initiatingService,*element,timespan,false,sessionPtr,ec,yield,mctx);
        if(ec) return;
        initiateRecalculation(initiatingService,sessionPtr,element->semantic,ec,yield,mctx);
    }
    void PdmService::recalculateProductFull(
            std::int64_t initiatingService,
            const std::string &product,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto prod = fetchRawNodeEntity(initiatingService,product,sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(prod->role != PdmRoles::Product) return;

        if(!prod->entity.has_value()) return;
        if(!prod->entity->data.has_value()) return;
        std::optional<long double> timespan;
        {
            WiPdmElementData data = fromJson<WiPdmElementData>(prod->entity->data.value());
            if(data.ster.has_value()){
                auto it = data.ster->data.find("expected_life_time");
                if(it != data.ster->data.end()){
                    auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                    if(gen_val!=nullptr){
                        auto val = std::get_if<std::optional<long double>>(gen_val);
                        if(val!=nullptr){
                            timespan = *val;
                        }
                    }
                }
            }
        }

        WiPdmRawNodeEntity::Container nodes;
        fetchRawNodesEntity(initiatingService,product,nodes,sessionPtr,ec,yield,mctx);
        if(ec)return;
        for(const auto&node:nodes){
            recalculateProductFullLayer(initiatingService,node,timespan,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        recalculateProduct(initiatingService,prod->semantic,sessionPtr,ec,yield,mctx);
        if(ec) return;

        // recalculate all schemas
        std::vector<std::string> schemas;
        std::vector<std::int64_t> roles = {PdmRoles::RbdSchema};
        auto proj = nodeNearestAncestor(initiatingService,prod->semantic,PdmRoles::Project,1,sessionPtr,ec,yield,mctx);
        if(ec) return;
        DataAccessConst().fetchPdmPaginatedView(schemas, proj.semantic, roles, 0, 0, std::numeric_limits<std::int64_t>::max(), mctx, ec, yield); // todo: как то придумать как сделать LIMIT ALL в query
        if(ec) return;
        for(const auto &schema:schemas){
            mctx.addSchemaTrigger(schema);
        }
    }
    void PdmService::recalculateProduct(
            std::int64_t initiatingService,
            const std::string &product,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::ignore_unused(initiatingService);
        GUARD_PDM_METHOD();

        std::shared_ptr<WiPdmRawNodeEntity> product_node = fetchRawNodeEntity(initiatingService,product, sessionPtr, ec, yield, mctx);
        if(ec || !product_node) return;
        WiPdmElementData data;
        if(product_node->entity.has_value()){
            if(product_node->entity->data.has_value()){
                data = fromJson<decltype(data)>(product_node->entity->data.value());
            }
        }
        
        WiUpdatePdmNodeQuery updateQueryProduct(*product_node);
        Number failure_rate = 0_Nr;
        bool calculated = false;
        WiPdmRawNodeEntity::Container nodes;
        fetchRawNodesEntity(initiatingService,product_node->semantic, nodes,sessionPtr,ec,yield,mctx);
        for(const auto &node: nodes){
            if(node.role != PdmRoles::ProxyComponent &&
               node.role != PdmRoles::Container && node.role != PdmRoles::ElectricComponent){
                continue;
            }
            if(!node.entity.has_value()) continue;
            if(!node.entity->data.has_value()) continue;
            WiPdmElementData node_data = fromJson<decltype(node_data)>(node.entity->data.value());
            if(!node_data.variables.has_value()) continue;
            if(!node_data.variables->failure_rate.has_value()) continue;
            failure_rate += node_data.variables->failure_rate.value();
            calculated = true;
        }

        WiPdmElementVariables vars;
        std::optional<long double> timespan;
        if(!calculated) {
            WI_LOG_INFO() << "product is not calculated";
        } else {
            if(data.ster.has_value()){
                auto it = data.ster->data.find("expected_life_time");
                if(it != data.ster->data.end()){
                    auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                    if(gen_val!=nullptr){
                        auto val = std::get_if<std::optional<long double>>(gen_val);
                        if(val!=nullptr){
                            timespan = *val;
                        }
                    }
                }
            }
            vars = calculateAll(failure_rate,timespan);
        }
        data.variables = vars;

        updateQueryProduct.data = toJson(data);
        updateQueryProduct.updateData = true;
        
        updateNode(initiatingService, updateQueryProduct, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }
    void PdmService::recalculateRbds(
            std::int64_t initiatingService,
            const std::string &rbd,
            const std::shared_ptr<IWiSession> sessionPtr,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::system::error_code ec;
        GUARD_PDM_METHOD();
        auto rbds_node = fetchRawNodeEntity(initiatingService,rbd, sessionPtr, ec, yield,mctx);
        if(ec) return;
        if(rbds_node->role != PdmRoles::ProjectRbds) return;

        WiPdmRawNodeEntity::Container rbd_nodes;
        fetchRawNodesEntity(initiatingService,rbds_node->semantic,rbd_nodes,sessionPtr,ec,yield,mctx);
        if(ec) return;

        // calculate all rbd schemas values
        for(const auto &rbd_node:rbd_nodes){
            recalculateRbd(initiatingService,rbd_node.semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }

    void PdmService::recalculateRbd(
            std::int64_t initiatingService,
            const std::string &rbd,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        auto rbd_node = fetchRawNodeEntity(initiatingService,rbd,sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(rbd_node->role != PdmRoles::RbdSchema) return;

        WiPdmElementData data;
        if(rbd_node->entity.has_value()) {
            if (rbd_node->entity->data.has_value()){
                data = fromJson<decltype(data)>(rbd_node->entity->data.value());
            }
        }

        WiUpdatePdmNodeQuery updateQueryRbd(*rbd_node);

        auto start = nodeNearestDescendant(initiatingService, rbd, PdmRoles::RbdInputNode, 1, sessionPtr, ec, yield, mctx);
        if(ec){
            WI_LOG_DEBUG() <<"RBD RECALCULATION FAILED " << ec.what();
            return;
        }

        auto end = nodeNearestDescendant(initiatingService, rbd, PdmRoles::RbdOutputNode, 1, sessionPtr, ec, yield, mctx);
        if(ec){
            WI_LOG_DEBUG() <<"RBD RECALCULATION FAILED " << ec.what();
            return;
        }

        // Обновляем переменные блоков схемы
        std::vector<std::string> container;
        DataAccessConst().fetchPdmPaginatedView(container, rbd, {PdmRoles::RbdBlock}, 0, 0, std::numeric_limits<std::int64_t>::max(), mctx, ec, yield);
        for (const auto &rbdBlockSemantic : container) {
            auto rbdBlockNode = fetchRawNode(initiatingService, rbdBlockSemantic, sessionPtr,ec, yield,mctx);
            if (ec) continue;
            if (rbdBlockNode->extension.has_value()) {
                const auto extension = fromJson<WiPdmRbdBlockExtension>(rbdBlockNode->extension.value());
                if (extension.ref.has_value()) {
                    fillRbdBlockVars(initiatingService, extension.ref.value(), rbdBlockSemantic, sessionPtr, ec, yield, mctx);
                }
            }
        }

        WiRbdChain chain;
        chain.source = start.semantic;
        chain.target = end.semantic;

        WiPdmElementVariables vars;
        std::optional<long double> timespan;

        WiPdmRbdExtension rbd_schemaExt = fromJson<decltype(rbd_schemaExt)>(rbd_node->extension.value());
        if(rbd_schemaExt.expected_life_time.has_value())
        {
            timespan = rbd_schemaExt.expected_life_time.value();
        }
        else
        {
            boost::system::error_code tec;
            auto proj_node = nodeNearestAncestor(initiatingService, rbd_node->semantic, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
            if(!tec) {
                auto prod_node_ = nodeNearestDescendant(initiatingService,
                                                        proj_node.semantic,
                                                        PdmRoles::Product,
                                                        1,
                                                        sessionPtr,
                                                        ec,
                                                        yield,
                                                        mctx);
                auto prod_node = fetchRawNodeEntity(initiatingService,prod_node_.semantic, sessionPtr, ec, yield, mctx);
                if (!tec) {
                    WiPdmElementData prod_data;
                    if(prod_node->entity.has_value()){
                        if(prod_node->entity->data.has_value()){
                            prod_data = fromJson<decltype(prod_data)>(prod_node->entity->data.value());
                        }
                    }
                    if (prod_data.ster.has_value()) {
                        auto it = prod_data.ster->data.find("expected_life_time");
                        if (it != prod_data.ster->data.end()) {
                            auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                            if (gen_val != nullptr) {
                                auto val = std::get_if<std::optional<long double>>(gen_val);
                                if (val != nullptr) {
                                    timespan = *val;
                                }
                            }
                        }
                    }
                }
            }
        }
        boost::system::error_code tec;
        auto model = getRbdChainModel(initiatingService, chain, sessionPtr, tec, yield, mctx);
        if(model.has_value()){
            auto m = RbdModel(details::RBDPartModel(std::move(model.value())));
            if(timespan.has_value()) {
                Number ts = Number(timespan.value());
                vars.reliability = m.calculate(ts);
                if(vars.reliability.has_value()) {
                    Number reliability = vars.reliability.value();
                    // Если под интегральное выражение равно единице, средняя наработка бесконечна
                    if(1_Nr == reliability) {
                        vars.MTBF = std::numeric_limits<Number>::infinity();
                    }
                    // Если под интегральное выражение равно нулю, то и средняя наработка равна нулю
                    else if(0_Nr == reliability) {
                        vars.MTBF = 0_Nr;
                    }
                    // Иначе берем интеграл
                    else {
                        // the commented out part is number of iterations and error tolerance(target)
                        //
                        auto f = [&m](Number t) {
                            auto r = m.calculate(t);
                            if (r.has_value()) {
                                if (isfinite(r.value())) {
                                    return r.value();
                                }
                            }
                            return 0_Nr;
                        };

                      // у обоих вариантов снизу одинаковые значения на двух тестовых схемах
//                    vars.MTBF = gauss_kronrod::integrate(
//                        f,
//                        "0"_Nr, std::numeric_limits<Number>::infinity(),
//                        30, // max number of iterations
//                        "1e-20"_Nr); // tolerance
                        auto refinements = WI_CONFIGURATION().read_settings<std::int32_t>(reliability_quadrature_refinements);
                        vars.MTBF = boost::math::quadrature::exp_sinh<Number>(refinements).integrate(f); // default range is 0 to +inf
                    }
                }
            }
        }else{
            WI_LOG_DEBUG() <<"RBD RECALCULATION - SCHEMA CONTAINS NO MODEL" << ec.what();
        }
        if(!timespan.has_value()){
            WI_LOG_DEBUG() <<"RBD RECALCULATION - NO TIMESPAN PROVIDED" << ec.what();
        }
        if(ec) return;


        fillRBDVars(vars);
        if(ec) return;
        data.variables = vars;

        updateQueryRbd.data = toJson(data);
        updateQueryRbd.updateData = true;

        updateNode(initiatingService,updateQueryRbd,sessionPtr,Filter::filterOn,ec,yield,mctx);
        if(ec){
            WI_LOG_DEBUG() <<"RBD RECALCULATION FAILED " << ec.what();
            return;
        }
        WI_LOG_DEBUG() <<"RBD SUCCESSFULLY RECALCULATED";
    }

    void PdmService::recalculateContainer(
            std::int64_t initiatingService,
            const std::string &container,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService);

        auto container_node = fetchRawNodeEntity(initiatingService,container, sessionPtr, ec, yield, mctx);
        if(ec || !container_node) return;

        // todo: error
        if(container_node->role != PdmRoles::Container) return;

        // todo: error
        if(!container_node->entity.has_value()) return;
        if(!container_node->entity->data.has_value()) return;

        WiPdmElementData data = fromJson<decltype(data)>(container_node->entity->data.value());
        WiUpdatePdmNodeQuery updateQueryContainer(*container_node);

//        // todo: error
//        if(!data.variables.has_value()) return;

        WiPdmRawNodeEntity::Container elements;
        fetchRawNodesEntity(initiatingService,container, elements, sessionPtr, ec, yield, mctx);
        if(ec) return;

        Number failure_rate = 0_Nr;
        bool calculated = false;
        for(const auto &element: elements){

            // skip all beside containers, components and proxies
            if(element.role != PdmRoles::ElectricComponent &&
                element.role != PdmRoles::Container &&
                element.role != PdmRoles::ProxyComponent) continue;

            // todo: flag element as invalid
            if(!element.entity.has_value()) continue;
            if(!element.entity->data.has_value()) continue;

            WiPdmElementData node_data = fromJson<decltype(data)>(element.entity->data.value());

            // todo: flag element as invalid
            if(!node_data.variables.has_value()) continue;
            if(!node_data.variables->failure_rate.has_value()) continue;


            failure_rate += node_data.variables->failure_rate.value();
            calculated = true;
        }

        std::optional<long double> timespan;
        WiPdmElementVariables vars;

        std::optional<Number> fr_result;
        if(calculated){
            boost::system::error_code tec;
            auto prod_node_ = nodeNearestAncestor(initiatingService,
                                                    container_node->semantic,
                                                    PdmRoles::Product,
                                                    1,
                                                    sessionPtr,
                                                  tec,
                                                    yield,
                                                    mctx);
            auto prod_node = fetchRawNodeEntity(initiatingService,prod_node_.semantic, sessionPtr, tec, yield, mctx);
            if (!tec) {
                WiPdmElementData prod_data;
                if(prod_node->entity.has_value()){
                    if(prod_node->entity->data.has_value()){
                        prod_data = fromJson<decltype(prod_data)>(prod_node->entity->data.value());
                    }
                }
                if (prod_data.ster.has_value()) {
                    auto it = prod_data.ster->data.find("expected_life_time");
                    if (it != prod_data.ster->data.end()) {
                        auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                        if (gen_val != nullptr) {
                            auto val = std::get_if<std::optional<long double>>(gen_val);
                            if (val != nullptr) {
                                timespan = *val;
                            }
                        }
                    }
                }
            }
            fr_result = failure_rate;
        }
        vars = calculateAll(fr_result,timespan);
        data.variables = vars;


        updateQueryContainer.data = toJson(data);
        updateQueryContainer.updateData = true;
        updateNode(initiatingService, updateQueryContainer, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;

    }


    void PdmService::initiateRecalculation(
            std::int64_t initiatingService,
            const std::shared_ptr<IWiSession> sessionPtr,
            const std::string &initiator,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto initiatorNode = fetchRawNodeEntity(initiatingService, initiator, sessionPtr,ec,yield,mctx);
        if(ec) return;
        boost::system::error_code dec;
        switch(initiatorNode->role){
            case PdmRoles::Container:
            case PdmRoles::ProxyComponent:
            case PdmRoles::ElectricComponent:{
                if(!initiatorNode->extension.has_value()){
                    ec = make_error_code(error::element_invalid);
                    return;
                }
                WiPdmElementExtension extension = fromJson<decltype(extension)>(initiatorNode->extension.value());

                boost::system::error_code tec;
                auto containerNode = nodeNearestAncestor(initiatingService,initiatorNode->semantic,PdmRoles::Container,1,sessionPtr,tec,yield,mctx);
                if(!tec){
                    mctx.addElementTrigger(containerNode.semantic);
                    initiateRecalculation(initiatingService,sessionPtr,containerNode.semantic,ec,yield,mctx);
                }else{
                    tec = boost::system::error_code();
                    auto product = nodeNearestAncestor(initiatingService,initiator,PdmRoles::Product,1,sessionPtr,tec,yield,mctx);
                    if(tec) return;
                    mctx.addElementTrigger(product.semantic);
                }

                if(!extension.rbd_block_refs.empty()){
                    for(const auto &schema_refs: extension.rbd_block_refs) {
                       const auto &schema = schema_refs.first;
                        initiateRecalculation(initiatingService,
                                              sessionPtr,
                                              schema,
                                              dec,
                                              yield,
                                              mctx);
                    }
                }
            }
                break;
            case PdmRoles::Product:{
                mctx.addProductTrigger(initiator);
            }
                break;
            case PdmRoles::RbdGroupStart:
            case PdmRoles::RbdGroupEnd:
            case PdmRoles::RbdOutputNode:
            case PdmRoles::RbdInputNode:
            case PdmRoles::RbdBlock:
            case PdmRoles::SubRbd:{
                auto rbdNode = nodeNearestAncestor(initiatingService,initiatorNode->semantic, PdmRoles::RbdSchema,1,sessionPtr,dec,yield,mctx);
                if(dec) return;
                mctx.addSchemaTrigger(rbdNode.semantic);
                mctx.addSchemaFlagsTrigger(rbdNode.semantic);
            }
            break;
            case PdmRoles::RbdSchema: {
                // todo: fixme - unused
                mctx.addSchemaTrigger(initiator);
                mctx.addSchemaFlagsTrigger(initiator);
            }
                break;
            default:
                // noop
                return;
        }
    }

        void PdmService::addNewNode(
                std::size_t initiatingService,
                WiNewPdmNodeQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                const Filter &filter,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const {
            GUARD_PDM_METHOD();
            addNewNodeInternal(initiatingService, query, sessionPtr, filter, ec, yield, mctx);
        }

        void PdmService::updateNode(
                std::size_t initiatingService,
                WiUpdatePdmNodeQuery &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                const Filter &filter,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const {
            GUARD_PDM_METHOD();
            std::int64_t nodeId = 0;
            auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});
            auto semantic = query.semantic;
            auto rawOldNodePtr = fetchRawNode(initiatingService,semantic, sessionPtr, ec, yield, mctx);
            if(ec) return;
            semantic = query.semantic;

            if(!query.updateRole){
                query.role = rawOldNodePtr->role;
            }
            if(!query.updateType){
                query.type = rawOldNodePtr->type;
            }
            if(!query.updateExtension){
                query.extension = rawOldNodePtr->extension;
        }

        if (rawOldNodePtr) {
            DataAccessConst().updatePdmNode(
                    nodeId,
                    query.semantic,
                    query.role,
                    query.type,
                    query.header,
                    query.description,
                    query.data,
                    query.updateData,
                    query.extension,
                    actor,
                    mctx,
                    ec,
                    yield
            );

            if(!ec) {
                // force update updated node
                auto newNodePtr = fetchRawNode(initiatingService,semantic, sessionPtr, ec, yield, mctx, true);
                auto newNodeEntityPtr = fetchRawNodeEntity(initiatingService,semantic, sessionPtr, ec, yield, mctx);
                if (!ec && newNodePtr) {
                    std::optional<std::string> parentOpt = std::nullopt;
                    constexpr static const char* semanticSplitter = "::";
                    std::vector<std::string> semantics;
                    boost::iter_split(semantics, semantic, boost::first_finder(semanticSplitter));

                    if (!semantics.empty()) {
                        semantics.erase(semantics.end() -1 );
                        std::string parent;
                        if (!semantics.empty()) {
                            buildSemantic(semantics, parent);
                            parentOpt = std::make_optional<std::string>(std::move(parent));
                        }
                    }

                    IWiPlatform::PdmUpdateNodeEvent event(m_eventNumerator);
                    // prohibit cache clearing onEvent
                    // since cahce already contains current version of the node
                    event.clearCached = false;
                    event.time_point = std::chrono::system_clock::now();
                    event.initiator = initiatingService;
                    event.userid = sessionPtr->userId();
//                    event.fio = sessionPtr->getUserView();
                    event.parent = parentOpt;
                    event.semantic = semantic;

                    if (query.role == PdmRoles::Project){
                        event.filter = Filter::filterOff;
                    }
                    else{
                        event.filter = filter;
                    }

                    event.dataChanged = query.updateData;
                    event.newNode = std::make_optional<WiPdmRawNode>(std::move(*newNodePtr));
                    if (newNodeEntityPtr)
                        event.newEntity = std::make_optional<WiPdmRawEntity>(std::move(*newNodeEntityPtr->entity));

                    event.oldNode = std::make_optional<WiPdmRawNode>(std::move(*rawOldNodePtr));
                    mctx.fire(std::forward<IWiPlatform::PdmUpdateNodeEvent>(event));
                }
            }
        }
        else {
            ec = make_error_code(code::node_not_found);
        }
//        initiateRecalculation(initiatingService,sessionPtr,semantic,ec,yield,mctx);
//        if(ec) return;
    }

    void PdmService::deleteNode(
        std::size_t initiatingService,
        const std::string &semantic,
        const std::shared_ptr<IWiSession> sessionPtr,
        const Filter &filter,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const {
        GUARD_PDM_METHOD();

        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});

        auto rawOldNodePtr = fetchRawNode(initiatingService, semantic, sessionPtr, ec, yield, mctx);

        //todo: also send delete event for all descendants
        initiateRecalculation(initiatingService,sessionPtr,rawOldNodePtr->semantic,ec,yield,mctx);
        if(ec) return;

        if (rawOldNodePtr) {
            DataAccessConst().deletePdmNode(semantic, actor, mctx, ec, yield);
            if(!ec) {
                std::optional<std::string> parentOpt = std::nullopt;
                constexpr static const char* semanticSplitter = "::";
                std::vector<std::string> semantics;
                boost::iter_split(semantics, semantic, boost::first_finder(semanticSplitter));

                if (!semantics.empty()) {
                    semantics.erase(semantics.end() -1 );
                    std::string parent;
                    if (!semantics.empty()) {
                        buildSemantic(semantics, parent);
                        parentOpt = std::make_optional<std::string>(std::move(parent));
                    }
                }

                IWiPlatform::PdmDeleteNodeEvent event(m_eventNumerator);
                // prohibit cache clearing onEvent
                // since cahce already contains current version of the node
                event.clearCached = false;
                event.time_point = std::chrono::system_clock::now();
                event.initiator = initiatingService;
                event.userid = sessionPtr->userId();
//                event.fio = sessionPtr->getUserView();
                event.parent = parentOpt;
                event.semantic = semantic;
                event.filter = filter;
                event.oldNode = std::make_optional<WiPdmRawNode>(*rawOldNodePtr);
                mctx.fire(std::forward<IWiPlatform::PdmDeleteNodeEvent>(event));
//                EventBusServiceSvc.fireAsync(std::forward<IWiPlatform::PdmDeleteNodeEvent>(event));
            }
        }
        else {
            ec = make_error_code(code::node_not_found);
        }
    }

    std::string PdmService::addNewNode(
        WiNewPdmNodeQuery &query,
        std::shared_ptr<WiSessionContext<IWiSession>> sessionContext,
        const Filter &filter,
        const net::yield_context &yield) const {

        auto sessionPtr = sessionContext->getSession();
        auto mctx = sessionContext->getMethodContext();
        auto &ec = mctx->ec();
        auto initiatingService = mctx->initiatingService();

        constexpr static const char *semanticSplitter = "::";
        std::int64_t nodeId = 0;
        std::string semantic = query.parent;
        std::string random = std::to_string((*m_mt)());

        semantic.append(semanticSplitter);
        semantic.append(random);

        std::string headerSemantic = semantic;
        headerSemantic.append(semanticSplitter);
        headerSemantic.append("header");

        std::string descriptionSemantic = semantic;
        descriptionSemantic.append(semanticSplitter);
        descriptionSemantic.append("description");

        auto entityStatus = std::make_optional<std::int64_t>(1);
        auto entityOldStatusData = std::optional<nlohmann::json>{};
        auto entityStatusData = std::optional<nlohmann::json>{};
        auto entityV = std::optional<std::int32_t>{1};
        auto nodeV = std::optional<std::int32_t>{1};
        auto nodeStatus = std::optional<std::int64_t>{1};
        auto nodeStatusData = std::optional<nlohmann::json>{};
        auto lockRole = std::optional<std::int32_t>{};
        auto lockVersion = std::optional<std::int32_t>{};
        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});

        auto parent = query.parent;
        auto selfSemantic = semantic;

        DataAccessConst().addPdmNode(
                nodeId, // id нового узла
                query.parent, // semantic родительского узла
                query.role, // role создаваемого узла
                semantic, // semantic нового узла
                headerSemantic, // semantic заголовка
                query.header, // заголовок
                descriptionSemantic, // semantic описания
                query.description, // описание
                query.data, // данные
                0, // old entity status
                entityStatus, // entityStatus,
                entityOldStatusData, //std::optional<nlohmann::json> &entityOldStatusData
                entityStatusData, //std::optional<nlohmann::json> &entityStatusData
                entityV, //entityV версия данных
                nodeV, //nodeV версия узла
                query.extension,
                query.type,
                actor, // актор
                nodeStatus, // статус
                nodeStatusData, // данные статуса
                lockRole, // role блокировки
                lockVersion, // версия блокировки
                *mctx,
                ec,
                yield);

        if (!ec) {
            auto rawNewNodePtr = fetchRawNode(selfSemantic, ec, yield, mctx);
            if (!ec && rawNewNodePtr) {
                IWiPlatform::PdmAddNodeEvent event(m_eventNumerator);
                event.time_point = std::chrono::system_clock::now();
                event.initiator = initiatingService;
                event.userid = sessionPtr->userId();
                event.parent = parent;
                event.semantic = selfSemantic;
                event.filter = filter;
                event.newNode = std::make_optional<WiPdmRawNode>(*rawNewNodePtr);
                mctx->fire(std::forward<IWiPlatform::PdmAddNodeEvent>(event));
            }
        }

        return selfSemantic;
    }

    void PdmService::updateNode(
        WiUpdatePdmNodeQuery &query,
        std::shared_ptr<WiSessionContext<IWiSession>> sessionContext,
        const Filter &filter,
        const net::yield_context &yield) const {

        auto mctx = sessionContext->getMethodContext();
        auto initiatingService = mctx->initiatingService();
        auto sessionPtr = sessionContext->getSession();
        auto &ec = mctx->ec();

        std::int64_t nodeId = 0;
        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});
        auto semantic = query.semantic;
        auto rawOldNodePtr = fetchRawNode(initiatingService,semantic, sessionPtr, ec, yield, mctx);
        if(ec) return;
        semantic = query.semantic;

        if(!query.updateRole){
            query.role = rawOldNodePtr->role;
        }
        if(!query.updateType){
            query.type = rawOldNodePtr->type;
        }
        if(!query.updateExtension){
            query.extension = rawOldNodePtr->extension;
        }

        if (rawOldNodePtr) {
            DataAccessConst().updatePdmNode(
                    nodeId,
                    query.semantic,
                    query.role,
                    query.type,
                    query.header,
                    query.description,
                    query.data,
                    query.updateData,
                    query.extension,
                    actor,
                    *mctx,
                    ec,
                    yield
            );

            if(!ec) {
                // force update updated node
                auto newNodePtr = fetchRawNode(initiatingService,semantic, sessionPtr, ec, yield, mctx, true);
                auto newNodeEntityPtr = fetchRawNodeEntity(initiatingService,semantic, sessionPtr, ec, yield, mctx);
                if (!ec && newNodePtr) {
                    std::optional<std::string> parentOpt = std::nullopt;
                    constexpr static const char* semanticSplitter = "::";
                    std::vector<std::string> semantics;
                    boost::iter_split(semantics, semantic, boost::first_finder(semanticSplitter));

                    if (!semantics.empty()) {
                        semantics.erase(semantics.end() -1 );
                        std::string parent;
                        if (!semantics.empty()) {
                            buildSemantic(semantics, parent);
                            parentOpt = std::make_optional<std::string>(std::move(parent));
                        }
                    }

                    IWiPlatform::PdmUpdateNodeEvent event(m_eventNumerator);
                    // prohibit cache clearing onEvent
                    // since cahce already contains current version of the node
                    event.clearCached = false;
                    event.time_point = std::chrono::system_clock::now();
                    event.initiator = initiatingService;
                    event.userid = sessionPtr->userId();
                    event.parent = parentOpt;
                    event.semantic = semantic;

                    if (query.role == PdmRoles::Project){
                        event.filter = Filter::filterOff;
                    }
                    else{
                        event.filter = filter;
                    }

                    event.dataChanged = query.updateData;
                    event.newNode = std::make_optional<WiPdmRawNode>(std::move(*newNodePtr));
                    if (newNodeEntityPtr)
                        event.newEntity = std::make_optional<WiPdmRawEntity>(std::move(*newNodeEntityPtr->entity));

                    event.oldNode = std::make_optional<WiPdmRawNode>(std::move(*rawOldNodePtr));
                    mctx->fire(std::forward<IWiPlatform::PdmUpdateNodeEvent>(event));
                }
            }
        }
        else {
            ec = make_error_code(code::node_not_found);
        }
    }

    void PdmService::deleteNode(
        const std::string &semantic,
        std::shared_ptr<WiSessionContext<IWiSession>> sessionContext,
        const Filter &filter,
        const net::yield_context &yield) const {

        auto mctx = sessionContext->getMethodContext();
        auto initiatingService = mctx->initiatingService();
        auto sessionPtr = sessionContext->getSession();
        auto &ec = mctx->ec();

        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});
        auto rawOldNodePtr = fetchRawNode(initiatingService, semantic, sessionPtr, ec, yield, mctx);

        if (rawOldNodePtr) {
            DataAccessConst().deletePdmNode(semantic, actor, *mctx, ec, yield);
            if(!ec) {
                std::optional<std::string> parentOpt = std::nullopt;
                constexpr static const char* semanticSplitter = "::";
                std::vector<std::string> semantics;
                boost::iter_split(semantics, semantic, boost::first_finder(semanticSplitter));

                if (!semantics.empty()) {
                    semantics.erase(semantics.end() -1 );
                    std::string parent;
                    if (!semantics.empty()) {
                        buildSemantic(semantics, parent);
                        parentOpt = std::make_optional<std::string>(std::move(parent));
                    }
                }

                IWiPlatform::PdmDeleteNodeEvent event(m_eventNumerator);
                // prohibit cache clearing onEvent
                // since cahce already contains current version of the node
                event.clearCached = false;
                event.time_point = std::chrono::system_clock::now();
                event.initiator = initiatingService;
                event.userid = sessionPtr->userId();
                event.parent = parentOpt;
                event.semantic = semantic;
                event.filter = filter;
                event.oldNode = std::make_optional<WiPdmRawNode>(*rawOldNodePtr);
                mctx->fire(std::forward<IWiPlatform::PdmDeleteNodeEvent>(event));
            }
        }
        else {
            ec = make_error_code(code::node_not_found);
        }
    }

    std::shared_ptr<WiPdmRawNode> PdmService::fetchRawNode(
        const std::string &semantic,
        std::shared_ptr<WiSessionContext<IWiSession>> sessionContext,
        const net::yield_context &yield) const noexcept(true) {
        auto mctx = sessionContext->getMethodContext();
        return WiCacheSvc.getAsync<WiPdmRawNode>(semantic, *mctx, mctx->ec(), yield);
    }

    std::shared_ptr<WiPdmRawNodeEntity> PdmService::fetchRawNodeEntity(
        const std::string &semantic,
        std::shared_ptr<WiSessionContext<IWiSession>> sessionContext,
        const net::yield_context &yield) const noexcept(true) {
        auto mctx = sessionContext->getMethodContext();
        return WiCacheSvc.getAsync<WiPdmRawNodeEntity>(semantic, *mctx, mctx->ec(), yield);
    }

    void PdmService::lockNode(
            std::size_t initiatingService,
            std::string semantic,
            std::int32_t role,
            bool propagate,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService);
        DataAccessConst().lockPdmNode(semantic, role, propagate, sessionPtr->userId(), mctx, ec, yield);
    }

    void PdmService::unlockNode(
            std::size_t initiatingService,
            std::string semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        boost::ignore_unused(initiatingService);

        DataAccessConst().unlockPdmNode(semantic, sessionPtr->userId(), mctx, ec, yield);
    }

    std::optional<WiSemanticsResult> PdmService::moveElementsInternal(
            std::size_t initiatingService,
            const WiMoveElementsQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        if(query.sources.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return std::nullopt;
        }
        // get parent sem
        std::string destination = query.destination;
//        bool push = false;
        if(query.prepend.has_value()){
            if(query.prepend.value()){
                destination = parentSemantic(query.destination, ec);
//                push = true;
                if(ec) return std::nullopt;
            }
        }
        WiSemanticsResult res;
        for(auto it = query.sources.begin(); it < query.sources.end(); ++it){
            auto dest_rel = checkSemanticsSameBranch(*it, destination);
            if(dest_rel.has_value()){
                if(dest_rel.value() <= 0){ // *it is parent of destination or is destination
                    ec = make_error_code(error::cant_move_element_inside_itself_or_it_descendants);
                    return std::nullopt;
                }
            }
            for(auto jt = std::next(it); jt < query.sources.end(); ++jt){
                auto rel = checkSemanticsSameBranch(*it, *jt);
                if(rel.has_value()){
                    ec = make_error_code(error::cant_move_element_and_its_descendants_to_the_same_destination_simultaneously);
                    return std::nullopt;
                }
            }
        }
        for(const auto &elem:query.sources) {
            //check that the elements are both in same project
            auto first_comp = nodeNearestAncestor(initiatingService,
                                                  destination,
                                                  PdmRoles::Project,
                                                  1,
                                                  sessionPtr,
                                                  ec,
                                                  yield,
                                                  mctx);
            if (ec) {
                ec = make_error_code(error::cant_move_element_outside_of_project);
                return std::nullopt;
            }
            auto second_comp = nodeNearestAncestor(initiatingService,
                                                   elem,
                                                   PdmRoles::Project,
                                                   1,
                                                   sessionPtr,
                                                   ec,
                                                   yield,
                                                   mctx);
            if (ec) {
                ec = make_error_code(error::cant_move_element_outside_of_project);
                return std::nullopt;
            }
            if (first_comp.semantic != second_comp.semantic) {
                ec = make_error_code(error::cant_move_element_outside_of_project);
                return std::nullopt;
            }

            auto node = fetchRawNodeEntity(initiatingService, elem, sessionPtr, ec, yield, mctx);
            if (!node || ec) {
                return std::nullopt;
            }
            WiAddComponentWithData checkQuery;
            checkQuery.destination = destination;
            if (node->entity.has_value()) {
                if (node->entity.value().data.has_value()) {
                    checkQuery.data = fromJson<WiPdmElementData>(node->entity.value().data.value());
                }
            }
            checkElementDestination(initiatingService,
                                    checkQuery,
                                    node->role,
                                    node->type,
                                    false,
                                    sessionPtr,
                                    ec,
                                    yield,
                                    mctx);
            if (ec) return std::nullopt;

            auto parent =parentSemantic(elem, ec);
            if (ec) return std::nullopt;

            if(parent != destination)
            {
                WiMovePdmNodeQuery moveQuery;
                moveQuery.destination = destination;
                moveQuery.semantic = elem;
                auto semantic = moveNodeInternal(initiatingService, moveQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if (ec || !semantic.has_value()) return std::nullopt;

                res.semantics.push_back(semantic.value().semantic);
            }
            else
            {
                res.semantics.push_back(elem);
            }
        }
        return res;
    }

    std::optional<WiSemanticsResult> PdmService::moveElements(
            std::size_t initiatingService,
            const WiMoveElementsQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend.has_value()){
            if(query.prepend.value()){
                destination = parentSemantic(query.destination, ec);
                if(ec) return std::nullopt;
            }
        }
        // todo: b4 moving check that none of the elements are not a parent to each other in any way, and that none of them a parent to destination
        auto res = moveElementsInternal(initiatingService, query, sessionPtr, ec, yield, mctx);
        if(ec || !res.has_value()){
            if(!ec){
                ec = make_error_code(error::cant_create_element);
            }
            return std::nullopt;
        }
        // now we need to build similar query
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = false;
        rearrangeQuery.destination = query.destination;
        rearrangeQuery.elements = std::move(res.value().semantics);
        if(query.prepend.has_value()){
            rearrangeQuery.prepend_mode = query.prepend.value();
        }
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        res.value().semantics = std::move(rearrangeQuery.elements);


        // now we need to rearrange all parents of moved out elements
        rearrangeQuery.elements = std::vector<std::string>();
        rearrangeQuery.prepend_mode = false;

        // not the best solution but good enough
        // if nodes that need to be updated are in a chain example("1", "1::2", "1::2::3")
        // we will rearrange all of the nodes, but actually by rearranging only the node 1 there is a chance that other
        // nodes will be rearranged to in the process, and we will check them second time.
        for(const auto &dest : std::set<std::string>(query.sources.begin(), query.sources.end()) ){
            std::string parent = parentSemantic(dest, ec);
            if(ec) return std::nullopt;
            if(parent.compare(destination)){ // not a destination
                rearrangeQuery.destination = parent;
                rearrangeQuery.prepend_mode = false;
                rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
                if(ec) return std::nullopt;
            }
        }
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addProject(
            std::size_t initiatingService,
            const WiNewProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        std::string PdmProjectsSemanticsUpdated(H_TO_S(StringHelper::PdmProjectsSemantics));
        auto nodeProjectsNodeForceUpdated =  fetchRawNode(
                                                initiatingService,
                                                PdmProjectsSemanticsUpdated,
                                                sessionPtr,
                                                ec,
                                                yield,
                                                mctx,
                                                true);

        WiUpdatePdmNodeQuery updateQueryProjects(*nodeProjectsNodeForceUpdated);

        std::int64_t projectsIndex;
        if(updateQueryProjects.extension.has_value())
        {
            auto ext = fromJson<WiPdmProjectExtension>(updateQueryProjects.extension.value());
            projectsIndex = ext.index;
        }
        else
        {
            projectsIndex = 0;
        }

        WiNewPdmNodeQuery newNodeQuery;
        newNodeQuery.header = std::move(query.header);
        newNodeQuery.description = std::move(query.description);

        if (query.data.has_value()) {
            newNodeQuery.data = query.data;
        }

        std::int64_t newProjectIndex;
        if (projectsIndex > 0)
        {
            newProjectIndex = projectsIndex + 1;
        }
        else
        {
            newProjectIndex = 1;
        }

        if (query.extension.has_value()) {
            auto ext = fromJson<WiPdmProjectExtension>(query.extension.value());
            ext.index = newProjectIndex;
            newNodeQuery.extension.value() = toJson(ext);
        }
        else
        {
            WiPdmProjectExtension ext;
            ext.index = newProjectIndex;
            newNodeQuery.extension = toJson(ext);
        }

        updateProjectsProjectIdBySystem(initiatingService,newProjectIndex,ec, yield, mctx);
        if (ec) return std::nullopt;

        newNodeQuery.parent = H_TO_S(StringHelper::PdmProjectsSemantics);
        newNodeQuery.role = PdmRoles::Project;
        auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);

        if (!ec) {
            WiNewPdmNodeQuery newProjectCompositionNodeQuery;
            newProjectCompositionNodeQuery.header = getDefaultPdmProjectCompositionHeader();
            newProjectCompositionNodeQuery.description = getDefaultPdmProjectCompositionDescription();
            newProjectCompositionNodeQuery.parent = selfSemantic;
            newProjectCompositionNodeQuery.role = PdmRoles::ProjectComposition;
            auto selfCompositionSemantic = addNewNodeInternal(initiatingService, newProjectCompositionNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
            if(ec) return std::nullopt;
            // add templates node
            WiNewPdmNodeQuery newProjectElementBinNodeQuery;
            WiPdmBinExtension binExtension;
            binExtension.index = 0u;
            newProjectElementBinNodeQuery.extension = toJson(binExtension);
            newProjectElementBinNodeQuery.header = getDefaultPdmProjectElementBinHeader();
            newProjectElementBinNodeQuery.description = getDefaultPdmProjectElementBinDescription();
            newProjectElementBinNodeQuery.parent = selfSemantic;
            newProjectElementBinNodeQuery.role = PdmRoles::ProjectElementBin;
            auto selfElementBinSemantic = addNewNodeInternal(initiatingService, newProjectElementBinNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
            // add settings node
            WiNewPdmNodeQuery newProjectSettingsNodeQuery;
            newProjectSettingsNodeQuery.header = getDefaultPdmProjectSettingsHeader();
            newProjectSettingsNodeQuery.description = getDefaultPdmProjectSettingsDescription();
            newProjectSettingsNodeQuery.parent = selfSemantic;
            newProjectSettingsNodeQuery.role = PdmRoles::ProjectSettings;
            auto selfSettingsSemantic = addNewNodeInternal(initiatingService, newProjectSettingsNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
            if(ec) return std::nullopt;
            // add settings::components
            WiNewPdmNodeQuery newProjectSettingComponentsNodeQuery;
            newProjectSettingComponentsNodeQuery.header = getDefaultPdmProjectSettingsComponentsHeader();
            newProjectSettingComponentsNodeQuery.description = getDefaultPdmProjectSettingsComponentsDescription();
            newProjectSettingComponentsNodeQuery.parent = selfSettingsSemantic;
            newProjectSettingComponentsNodeQuery.role = PdmRoles::ProjectSettingsComponents;
            auto selfSettingComponentsSemantic = addNewNodeInternal(initiatingService, newProjectSettingComponentsNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
            if(ec) return std::nullopt;
            // add settings containers

            //add RBDS
            WiNewPdmNodeQuery newProjectRbdSNodeQuery;
            newProjectRbdSNodeQuery.header = getDefaultPdmProjectRbdsHeader();
            newProjectRbdSNodeQuery.description = getDefaultPdmProjectRbdsDescription();
            newProjectRbdSNodeQuery.parent = selfSemantic;
            newProjectRbdSNodeQuery.role = PdmRoles::ProjectRbds;
            {
                WiPdmModuleExtension RbdSExtension;
                RbdSExtension.index = 1;
                newProjectRbdSNodeQuery.extension = toJson(RbdSExtension);
            }
            auto selfRbdSSemantic = addNewNodeInternal(initiatingService, newProjectRbdSNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
            if(ec) return std::nullopt;

            WiNewRbd newRbdQuery;
            newRbdQuery.semantic = selfSemantic;

            // add default product
            WiNewProduct newProd;
            newProd.parent = selfCompositionSemantic;

            newProd.header = getDefaultPdmProductHeader();
            newProd.description = getDefaultPdmProductDescription();
            auto prod = addProduct(initiatingService, newProd, sessionPtr, ec, yield, mctx);
            if(ec) return std::nullopt;

            auto prod_node = fetchRawNodeEntity(initiatingService,prod->semantic,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            WiPdmElementData data = fromJson<WiPdmElementData>(prod_node->entity->data.value());
            if(data.ster.has_value()){
                auto it = data.ster->data.find("expected_life_time");
                if(it != data.ster->data.end()){
                    auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                    if(gen_val!=nullptr){
                        auto val = std::get_if<std::optional<long double>>(gen_val);
                        if(val!=nullptr){
                            //пока захардкодим.
                            newRbdQuery.expected_life_time = std::to_string(100000.0);
                        }
                    }
                }
            }

            addRbd(initiatingService,newRbdQuery,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            //Add FMEA
            addFmeaModule(initiatingService, selfSemantic, sessionPtr, ec, yield, mctx);
            if(ec) return std::nullopt;

            WiNewPdmNodeQuery newDictionariesNodeQuery;
            newDictionariesNodeQuery.role = PdmRoles::Dictionaries;
            newDictionariesNodeQuery.parent = selfSemantic;
            auto dictionariesSemantic = addNewNodeInternal(initiatingService, newDictionariesNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return std::nullopt;

            WiNewPdmNodeQuery newFailureTypeDictionaryNodeQuery;
            newFailureTypeDictionaryNodeQuery.role = PdmRoles::FailureTypeDictionary;
            newFailureTypeDictionaryNodeQuery.parent = dictionariesSemantic;
            addNewNodeInternal(initiatingService, newFailureTypeDictionaryNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return std::nullopt;

            if(!ec) {
                WiSemanticResult result;
                result.semantic = std::move(selfSemantic);
                return std::make_optional<WiSemanticResult>(std::move(result));
            } else {
                return std::nullopt;
            }
        }
        else {
            return std::nullopt;
        }
    }

    void PdmService::deleteElementInternal(
            std::size_t initiatingService,
            const std::string & semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        auto node = fetchRawNode(initiatingService,semantic, sessionPtr, ec, yield, mctx);
        if(ec || !node) return;
        switch(node->role) {
            case PdmRoles::ElectricComponent:
            case PdmRoles::Container:
            case PdmRoles::BlankComponent:
            case PdmRoles::ProxyComponent:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return;
                break;
        }


        auto projNode = nodeNearestAncestor(initiatingService,semantic,PdmRoles::Project,1,sessionPtr,ec,yield,mctx);
        if(ec) return;
        auto _prodNode = nodeNearestDescendant(initiatingService,projNode.semantic,PdmRoles::Product,1,sessionPtr,ec,yield,mctx);
        if(ec) return;
        auto prod = fetchRawNodeEntity(initiatingService,_prodNode.semantic,sessionPtr,ec,yield,mctx);
        if(!prod || ec) return;
        WiUpdatePdmNodeQuery updateQueryProduct(*prod);
        WiPdmElementData prod_data;
        if(ec) return;
        if (prod->entity.has_value()) {
            if (prod->entity.value().data.has_value()) {
                prod_data = fromJson<WiPdmElementData>(prod->entity.value().data.value());
            }
        }

        removeElementFailureTypes(initiatingService, semantic, sessionPtr, ec, yield, mctx);
        if (ec) return;

        deleteElementInternal_impl(initiatingService,*node,prod_data,sessionPtr,ec,yield,mctx);
        if(ec) return;
        deleteNode(initiatingService, node->semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;


        updateQueryProduct.data = toJson(prod_data);
        updateQueryProduct.updateData = true;
        updateNode(initiatingService, updateQueryProduct, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;

//        std::string parent = parentSemantic(semantic, ec);
//        if(ec) return;

    }
    void PdmService::deleteElementInternal_impl(
            std::size_t initiatingService,
            const WiPdmRawNode&node,
            WiPdmElementData&prod_data,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        if(node.role == PdmRoles::Container){
            WiPdmRawNode::Container container;
            fetchRawNodes(initiatingService,node.semantic,container,sessionPtr,ec,yield,mctx);
            if(ec) return;
            for(const auto&descendant:container){
                deleteElementInternal_impl(initiatingService,descendant,prod_data,sessionPtr,ec,yield,mctx);
                if(ec) return;
            }
        }

        switch(node.role){
            case PdmRoles::ElectricComponent:
            case PdmRoles::Container:
            case PdmRoles::ProxyComponent:{
                if(node.extension.has_value()) {
                    WiPdmElementExtension ext = fromJson<WiPdmElementExtension>(node.extension.value());
                    std::string old_data = ext.letter_tag;

                    old_data.erase(std::remove_if(old_data.begin(), old_data.end(), isspace), old_data.end());
                    auto rit2 = std::find_if_not(old_data.rbegin(), old_data.rend(), isdigit);
                    std::string letter_part_old_data(old_data.begin(), rit2.base());
                    std::string st_digital_part_old_data(rit2.base(), old_data.end());

                    std::int32_t digital_part_old_data = atoi(st_digital_part_old_data.c_str());

                    if (prod_data.positional_indexies_cache.find(letter_part_old_data)
                        != prod_data.positional_indexies_cache.end() and digital_part_old_data != 0) {
                        prod_data.positional_indexies_cache[letter_part_old_data].erase(digital_part_old_data); //чистка
                    }
                }
            }
                break;
        }
    }

    void PdmService::deleteElementsFromBin(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        if(query.semantics.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        {
            std::set<std::string> elems;
            auto it = query.semantics.begin();
            elems.insert(*it);
            auto parent = parentSemantic(*(it++),ec);
            if(ec) return;
            checkNode(initiatingService, parent, PdmRoles::ProjectElementBin, sessionPtr,  ec, yield, mctx);
            for(;it < query.semantics.end(); ++it){
                if(!elems.count(*it)) {
                    auto parent_sem = parentSemantic(*it, ec);
                    if (ec) return;
                    if (parent_sem != parent) {
                        ec = make_error_code(error::element_invalid);
                        return;
                    }
                    elems.insert(*it);
                }
            }

            for(const auto &elem : elems){
                deleteElementInternal(initiatingService, elem, sessionPtr,ec,yield,mctx);
                if(ec) return;
            }
        }
    }

    void PdmService::deleteAllElementsFromBin(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        {

            std::vector<std::int64_t> roles = {
                    PdmRoles::FunctionalUnit,
                    PdmRoles::RbdSchema,
                    PdmRoles::ElectricComponent,
                    PdmRoles::Container,
                    PdmRoles::ProxyComponent
            };

            WiPdmRawNode trash = nodeNearestDescendant(initiatingService,semantic,PdmRoles::ProjectElementBin,0,sessionPtr,ec,yield,mctx);
            if(ec) return;

            WiPdmRawNode::Container nodes;
            fetchRawNodes(initiatingService,trash.semantic, nodes,sessionPtr, ec, yield, mctx);

            std::for_each(begin(nodes), end(nodes), [this,&initiatingService,&sessionPtr,&ec,&yield,&mctx](WiPdmRawNode el_info)
            {
                deleteElementInternal(initiatingService, el_info.semantic, sessionPtr,ec,yield,mctx);
                if(ec) return;
            });
        }
    }

    void PdmService::deleteElementsDeprecated(
        std::size_t initiatingService,
        const WiSemanticsOnlyQuery &query,
        const std::shared_ptr<IWiSession> sessionPtr,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        if(query.semantics.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        std::set<std::string> parents;
        {
            std::set<std::string> elems;
            for(auto it = query.semantics.begin();it < query.semantics.end(); ++it){
                bool keep = true;
                for(auto jt = std::next(it); jt < query.semantics.end(); ++jt){
                    auto val = checkSemanticsSameBranch(*it,*jt);
                    if(val.has_value())
                        if(val.value()>=0){ // if same or jt is a parent of it
                            keep = false; break;
                        }
                }
                if(keep){
                    elems.insert(*it);
                }
            }
            if(elems.size() == 0 ){
                ec = make_error_code(error::no_work_to_do);
                return;
            }
            for(const auto &elem : elems){
                std::string parent = parentSemantic(elem, ec);
                if(ec) return;
                deleteElementInternal(initiatingService, elem, sessionPtr,ec,yield,mctx);
                if(ec) return;
                parents.insert(parent);
            }
        }
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = false;
        for(const auto &parent:parents){
            rearrangeQuery.destination = parent;
            rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
            if(ec) return;
        }
    }

    void PdmService::deleteProject(
            std::size_t initiatingService,
            const std::string & semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        checkNode(initiatingService, semantic, PdmRoles::Project, sessionPtr,  ec, yield, mctx);
        if(ec) return;
        deleteNode(initiatingService, semantic, sessionPtr, Filter::filterOff, ec, yield, mctx);
    }

    std::optional<WiPdmElementVariables> PdmService::getRbdBlockElementVars(
            std::size_t initiatingService,
            std::shared_ptr<WiPdmRawNodeEntity> block,
            const std::optional<long double> &timespan,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(block->role != PdmRoles::RbdBlock){
            ec = make_error_code(error::invalid_node_role);
            return std::nullopt;
        }
        if(!block->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }
        WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(block->extension.value());
        if(!extension.ref.has_value()){
            return std::nullopt;
        }

        auto element = fetchRawNodeEntity(initiatingService, extension.ref.value(), sessionPtr,ec,yield,mctx);
        if(ec || ! element)return std::nullopt;

        if(element->role != PdmRoles::ProxyComponent &&
            element->role != PdmRoles::ElectricComponent &&
            element->role != PdmRoles::Container){
            ec = make_error_code(error::invalid_node_role);
            return std::nullopt;
        }

        if(!element->entity.has_value()) {
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        if(!element->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        WiPdmElementData data = fromJson<decltype(data)>(element->entity->data.value());

        if(!data.variables.has_value()){
            // оказывается что бывают валидные элементы без лямбды, которые надо игнорировать, например пустой контейнер :P
            return std::nullopt;
        }
        fillAllVars(data.variables.value(),timespan,ec);
        return data.variables;
    }

    std::optional<WiPdmElementVariables> PdmService::getSubRbdRefSchemaVars(
            std::size_t initiatingService,
            std::shared_ptr<WiPdmRawNodeEntity> sub,
            const std::optional<long double> &timespan,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(sub->role != PdmRoles::SubRbd){
            ec = make_error_code(error::invalid_node_role);
            return std::nullopt;
        }
        if(!sub->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }
        WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(sub->extension.value());
        if(!extension.ref.has_value()){
            return std::nullopt;
        }

        auto schema = fetchRawNodeEntity(initiatingService, extension.ref.value(), sessionPtr,ec,yield,mctx);
        if(ec || ! schema)return std::nullopt;

        if(schema->role != PdmRoles::RbdSchema){
            ec = make_error_code(error::invalid_node_role);
            return std::nullopt;
        }

        if(!schema->entity.has_value()) {
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        if(!schema->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        WiPdmElementData data = fromJson<decltype(data)>(schema->entity->data.value());

        if(!data.variables.has_value()){
            // схема может быть не рассчитана
            return std::nullopt;
        }
        fillAllVars(data.variables.value(),timespan,ec);
        return data.variables;
    }

    std::optional<details::ReservedModelPtr> PdmService::getRbdReservedModel(
            std::size_t initiatingService,
            const WiRbdChain &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto start_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr,ec,yield,mctx);
        if(ec || !start_node) return std::nullopt;

        if(start_node->role != PdmRoles::RbdGroupStart){
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }

        std::vector<std::string> outputs;
        getRbdElementOutputs(initiatingService,start_node,outputs,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        details::ReservedModelPtr model = std::make_unique<details::ReservedModel>();
        bool calculated = false;
        for(const auto& output: outputs){
            WiRbdChain chain;
            chain.source = output;
            chain.target = query.target;

            auto ch_model = getRbdChainModel(initiatingService,chain, sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            if(ch_model.has_value()){
                model->chains.push_back(details::RBDPartModel(std::move(ch_model.value())));
                calculated = true;
            }
        }
        if(!calculated) {
            return std::nullopt;
        }

        return std::make_optional(std::move(model));
    }

    std::optional<details::LinearModelPtr> PdmService::getRbdChainModel(
            std::size_t initiatingService,
            const WiRbdChain &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto start_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr, ec, yield, mctx);
        if (ec || !start_node) return std::nullopt;

        auto end_node = fetchRawNodeEntity(initiatingService,query.target,sessionPtr, ec, yield, mctx);
        if (ec || !end_node) return std::nullopt;

        details::LinearModelPtr model = std::make_unique<details::LinearModel>();
        bool calculated = false;
        do {
            if (start_node->role == PdmRoles::RbdBlock) {
                std::optional<long double> timespan;
                auto bv = getRbdBlockElementVars(initiatingService, start_node,timespan, sessionPtr, ec, yield, mctx);

                // todo: flag element and exit
                if (ec) return std::nullopt;
                if(bv.has_value()) {
                    if(bv->failure_rate.has_value()){
                        model->chain.push_back(details::RBDPartModel(details::Node(bv->failure_rate.value())));
                        calculated = true;
                    }
                }
            }
            if (start_node->role == PdmRoles::SubRbd) {
                std::optional<long double> timespan;
                auto sv = getSubRbdRefSchemaVars(initiatingService, start_node,timespan, sessionPtr, ec, yield, mctx);

                // todo: flag element and exit
                if (ec) return std::nullopt;
                if(sv.has_value()) {
                    if(sv->failure_rate.has_value()){
                        model->chain.push_back(details::RBDPartModel(details::Node(sv->failure_rate.value())));
                        calculated = true;
                    }
                }
            }
            else if (start_node->role == PdmRoles::RbdGroupStart) {
                auto group = getRbdGroup(initiatingService,start_node->semantic, sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;

                auto gr_model = getRbdReservedModel(initiatingService, group.value(),  sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;
                if(gr_model.has_value()) {
                    model->chain.push_back(details::RBDPartModel(std::move(gr_model.value())));
                    calculated = true;
                }

                start_node = fetchRawNodeEntity(initiatingService,group.value().target,sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;
            }

            // end loop condition (or at least one thats not a error
            if(start_node->semantic == end_node->semantic) break;

            std::optional<std::string> output;
            getRbdElementOutput(initiatingService, start_node, output, sessionPtr, ec, yield, mctx);
            if (ec) return std::nullopt;

            if (!output.has_value()) {
                ec = make_error_code(error::invalid_rbd_element);
                return std::nullopt;
            }

            start_node = fetchRawNodeEntity(initiatingService,output.value(),sessionPtr, ec, yield, mctx);
            if (ec) return std::nullopt;

        } while(true);
        if(!calculated) return std::nullopt; // not an error, just no value
        return std::make_optional(std::move(model));
    }

    void PdmService::provisionRbdFlags(
            std::size_t initiatingService,
            const std::string &schema,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto schemaNode = fetchRawNode(initiatingService,schema,sessionPtr,ec,yield,mctx);
        if(ec||!schemaNode) return;
        if(schemaNode->role != PdmRoles::RbdSchema){
            return;
        }

        WiPdmRbdExtensionFlags flags;
        flags.contains_duplicates = false;
        flags.empty_blocks = false;
        flags.blocks_w_elements_wo_parameters = false;

        WiPdmRawNodeEntity::Container nodes;
        fetchRawNodesEntity(initiatingService,schema,nodes,sessionPtr,ec,yield,mctx);
        std::set<std::string> elements;
        while(!nodes.empty()){
            if(flags.contains_duplicates && flags.empty_blocks && flags.blocks_w_elements_wo_parameters) {
                nodes.clear();
                continue;
            }
            auto node = nodes.back();
            nodes.pop_back();
            if(node.role == PdmRoles::RbdBlock){
                if(!node.extension.has_value()){
                    flags.empty_blocks = true;
                    continue;
                }
                WiPdmRbdBlockExtension blockExt = fromJson<WiPdmRbdBlockExtension>(node.extension.value());
                if(!blockExt.ref.has_value()){
                    flags.empty_blocks = true;
                    continue;
                }else{
                    if(elements.count(blockExt.ref.value())){
                        flags.contains_duplicates = true;
                        continue;
                    }else{
                        elements.insert(blockExt.ref.value());
                    }
                }
                std::optional<long double> placeholder;

                auto _node = fetchRawNodeEntity(initiatingService,node.semantic,sessionPtr,ec,yield,mctx);
                if(ec) return;
                auto vars = getRbdBlockElementVars(initiatingService,_node,placeholder,sessionPtr,ec,yield,mctx);
                if(ec) return;
                if(!vars.has_value()) {
                    flags.blocks_w_elements_wo_parameters = true;
                    continue;
                }
                if(!vars->reliability.has_value()
                or !vars->failure_probability.has_value()){
                    flags.blocks_w_elements_wo_parameters = true;
                    continue;
                }
            }
            if(node.role == PdmRoles::SubRbd)
            {
                if(!node.extension.has_value()){
                    flags.empty_blocks = true;
                    continue;
                }
                WiPdmSubRbdExtension subExt = fromJson<WiPdmSubRbdExtension>(node.extension.value());
                if(!subExt.ref.has_value()){
                    flags.empty_blocks = true;
                    continue;
                }else{
                    if(elements.count(subExt.ref.value())){
                        flags.contains_duplicates = true;
                        continue;
                    }else{
                        elements.insert(subExt.ref.value());
                    }
                }
                std::optional<long double> placeholder;

                auto _node = fetchRawNodeEntity(initiatingService,node.semantic,sessionPtr,ec,yield,mctx);
                if(ec) return;
                auto vars = getSubRbdRefSchemaVars(initiatingService,_node,placeholder,sessionPtr,ec,yield,mctx);
                if(ec) return;
                if(!vars.has_value()) {
                    flags.subs_w_not_calculated_schemas = true;
                    continue;
                }
                if(!vars->reliability.has_value()
                or !vars->failure_probability.has_value()){
                    flags.subs_w_not_calculated_schemas = true;
                    continue;
                }
            }
        }
        WiUpdatePdmNodeQuery updateSchemaQuery(*schemaNode);
        WiPdmRbdExtension ext;
        if(schemaNode->extension.has_value()){
            ext = fromJson<WiPdmRbdExtension>(schemaNode->extension.value());
        }
        ext.flags = flags;
        updateSchemaQuery.extension = toJson(ext);
        updateSchemaQuery.updateExtension = true;

        updateNode(initiatingService,updateSchemaQuery,sessionPtr,Filter::filterOn,ec,yield,mctx);
    }

    std::optional<WiSemanticResult> PdmService::addRbd(
            std::size_t initiatingService,
            const WiNewRbd &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.semantic, PdmRoles::Project,sessionPtr,  ec, yield, mctx);
        if (ec) return std::nullopt;

        auto RbdsNode = nodeNearestDescendant(initiatingService, query.semantic,
                                                         PdmRoles::ProjectRbds, 1, sessionPtr, ec,
                                                         yield, mctx);
        if(ec) return std::nullopt;
        WiPdmModuleExtension RbdSExtension;
        WiPdmRbdExtension RbdExtension;
        if(!RbdsNode.extension.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        RbdSExtension = fromJson<decltype(RbdSExtension)>(RbdsNode.extension.value());
        RbdExtension.index = RbdSExtension.index++;

        if(!query.expected_life_time.has_value())
        {
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }

        if(!query.expected_life_time.value().empty())
        {
            RbdExtension.expected_life_time = std::stold(query.expected_life_time.value());
        }
        else
        {
            auto prod = nodeNearestDescendant(initiatingService, query.semantic,
                                                         PdmRoles::Product, 1, sessionPtr, ec,
                                                         yield, mctx);

            if(ec) return std::nullopt;
            auto prod_node = fetchRawNodeEntity(initiatingService,prod.semantic, sessionPtr, ec, yield, mctx);
            if(ec) return std::nullopt;
            if(!prod_node->entity.has_value()) return std::nullopt;
            if(!prod_node->entity->data.has_value()) return std::nullopt;

            {
                WiPdmElementData data = fromJson<WiPdmElementData>(prod_node->entity->data.value());
                if(data.ster.has_value()){
                    auto it = data.ster->data.find("expected_life_time");
                    if(it != data.ster->data.end()){
                        auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                        if(gen_val!=nullptr){
                            auto val = std::get_if<std::optional<long double>>(gen_val);
                            if(val!=nullptr){
                                RbdExtension.expected_life_time = *val;;
                            }
                        }
                    }
                }
            }
        }

        WiUpdatePdmNodeQuery updateRbdSNodeQuery(RbdsNode);
        updateRbdSNodeQuery.extension = toJson(RbdSExtension);
        updateRbdSNodeQuery.updateExtension = true;
        updateNode(initiatingService, updateRbdSNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }
        std::string suffix;
        try {
            suffix = " №" + boost::lexical_cast<std::string>(RbdExtension.index);
        } catch(...){
            ec = make_error_code(error::cant_create_element);
            return std::nullopt;
        }
        WiNewPdmNodeQuery newNodeQuery;
        if(query.header.has_value()){
            newNodeQuery.header = query.header.value();
            if (query.description.has_value())
            {
                newNodeQuery.description = query.description.value();
            }
            else{
                newNodeQuery.description = emptyTranslateText();
            }

        }
        else{
            if (!query.description.has_value())
            {
                newNodeQuery.header      = appendSuffixToTranslateText(getDefaultPdmRbdHeader(),suffix);
                newNodeQuery.description = appendSuffixToTranslateText(getDefaultPdmRbdDescription(),suffix);
            }
            else{
                ec = make_error_code(error::invalid_input_data);
                return std::nullopt;
            }
        }

        WiPdmElementData data;
        data.variables = WiPdmElementVariables{};
        newNodeQuery.parent = RbdsNode.semantic;
        newNodeQuery.role = PdmRoles::RbdSchema;
        newNodeQuery.extension = toJson(RbdExtension);
        newNodeQuery.data = toJson(data);
        auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }

        // addInput and oputput nodes
        WiPdmRbdInputNodeExtension rbdInputNodeExtension;

        WiNewPdmNodeQuery inputRbdNodeQuery;
        inputRbdNodeQuery.parent = selfSemantic;
        inputRbdNodeQuery.role = PdmRoles::RbdInputNode;
        inputRbdNodeQuery.extension = toJson(rbdInputNodeExtension);
        auto selfInputSemantic = addNewNodeInternal(initiatingService, inputRbdNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }

        WiPdmRbdOutputNodeExtension rbdOutputNodeExtension;
        WiNewPdmNodeQuery outputRbdNodeQuery;
        outputRbdNodeQuery.parent = selfSemantic;
        outputRbdNodeQuery.role = PdmRoles::RbdOutputNode;
        outputRbdNodeQuery.extension = toJson(rbdOutputNodeExtension);
        auto selfOutputSemantic = addNewNodeInternal(initiatingService, outputRbdNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) {
            return std::nullopt;
        }

        WiRbdLink link;
        link.source = selfInputSemantic;
        link.target = selfOutputSemantic;
        addRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        WiSemanticResult result;
        result.semantic = std::move(selfSemantic);
        return std::make_optional<WiSemanticResult>(std::move(result));
    }

    std::optional<WiFmeaTemplate::Container> PdmService::fetchFmeasTemplates(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        boost::ignore_unused(semantic);

        // Add default templates in group
        WiFmeaTemplate::Container tmpls;
        const auto container = MdmSvcConst.fetchNodesEntityView(initiatingService, H_TO_S(StringHelper::MdmReliabilityFmeaTemplatesSemantic), language, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;

        if(container.has_value()){
            for(const auto &node : container.value()){
                for(const auto &entity : node.entities){
                    if(entity.status.type != MdmDocumentStatusType::DEPRECATED){
                        if(!entity.data.has_value()) continue;
                        WiFmeaTemplate tmpl;
                        tmpl.name = node.name;
                        tmpl.semantic = node.semantic;
                        tmpl.revision = entity.version;
                        tmpl.tableTemplate = entity.data;
                        tmpls.push_back(tmpl);
                    }
                }
            }
        }

        // Add users templates in group
        //Пока пользовательских шаблонов нет, но в дальнейшем появятся.
        return std::make_optional<WiFmeaTemplate::Container>(std::move(tmpls));
    }

    std::optional<WiSemanticResult> PdmService::addFmeaSheet(
            std::size_t initiatingService,
            const WiAddFmeaSheetQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.parent, PdmRoles::ProjectFmeas, sessionPtr,  ec, yield, mctx);
        if (ec) return std::nullopt;

        auto sheetsNode = nodeNearestDescendant(initiatingService, query.parent,
                                                         PdmRoles::FmeaSheets, 1, sessionPtr, ec,
                                                         yield, mctx);
        if (ec) return std::nullopt;

        WiNewPdmNodeQuery newNodeQuery;
        newNodeQuery.parent = sheetsNode.semantic;
        newNodeQuery.role = PdmRoles::FmeaSheet;

        if (!query.header.has_value()) {
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }
        else {
            newNodeQuery.header = query.header.value();
            if (query.description.has_value()) {
                newNodeQuery.description = query.description.value();
            }
            else {
                newNodeQuery.description = emptyTranslateText();
            }
        }

        if (!query.tableSemantic.has_value()) {
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }
        checkNode(initiatingService, query.tableSemantic.value(), PdmRoles::FmeaTemplates, sessionPtr,  ec, yield, mctx);
        if (ec == make_error_code(wi::lib::database::error::pdm_node_not_found)) {
            //Если не пользовательский, то попробуем найти среди стандартных
            auto lang = std::optional(m_defaultLanguage);
            const auto tmpls = MdmSvcConst.fetchNodesEntityView(initiatingService, H_TO_S(StringHelper::MdmReliabilityFmeaTemplatesSemantic), lang, sessionPtr, ec, yield, mctx);
            if(!tmpls.has_value()){
                ec = make_error_code(error::node_entity_not_found);
                return std::nullopt;
            }
            const auto it = std::find_if(tmpls.value().cbegin(), tmpls.value().cend(), [&query](auto tmpl){return tmpl.semantic==query.tableSemantic.value();});
            if (it == tmpls->cend()) {
                ec = make_error_code(error::invalid_input_data);
                return std::nullopt;
            }
        }
        if (ec) {
            return std::nullopt;
        }

        WiSheetData data;
        data.templateSemantic = std::move(query.tableSemantic.value());
        newNodeQuery.data = toJson<decltype(data)>(data);

        if (!sheetsNode.extension.has_value()) {
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        auto sheetsExtension = fromJson<WiPdmModuleExtension>(sheetsNode.extension.value());
        WiPdmModuleExtension sheetExtension;
        sheetExtension.index = sheetsExtension.index++;
        newNodeQuery.extension = toJson<decltype(sheetExtension)>(sheetExtension);

        auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec){
            return std::nullopt;
        }

        WiSemanticResult result;
        result.semantic = std::move(selfSemantic);
        return std::make_optional<WiSemanticResult>(std::move(result));
    }

    void PdmService::deleteFmeaSheet(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.semantic, PdmRoles::FmeaSheet, sessionPtr,  ec, yield, mctx);
        if(ec) return;
        deleteNode(initiatingService, query.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    std::optional<WiFmeasSheet::Container> PdmService::fetchFmeasSheetsView(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.semantic, PdmRoles::Project, sessionPtr,  ec, yield, mctx);
        if (ec) return std::nullopt;
        auto sheetsNode = nodeNearestDescendant(initiatingService, query.semantic,
                                                         PdmRoles::FmeaSheets, 2, sessionPtr, ec,
                                                         yield, mctx);
        if (ec) return std::nullopt;

        WiFmeasSheet::Container result;
        WiPdmRawNodeEntity::Container container;
        fetchRawNodesEntity(initiatingService, sheetsNode.semantic, container, sessionPtr, ec, yield, mctx);
        if(ec || container.empty()) return std::nullopt;

        for(const auto &node : container) {
            if(node.role != PdmRoles::FmeaSheet) continue;
            WiFmeasSheet sheet;
            sheet.semantic = node.semantic;
            sheet.header = node.header;
            sheet.description = node.description;
            if (!node.entity.has_value()) {
                ec = make_error_code(error::node_entity_data_empty);
                return std::nullopt;
            }
            const auto entity = node.entity.value();
            if (!entity.data.has_value()) {
                ec = make_error_code(error::node_entity_data_empty);
                return std::nullopt;
            }
            sheet.tableTemplate = fromJson<WiSheetData>(entity.data.value()).templateSemantic;
            sheet.type = node.type;
            sheet.actor = node.actor;
            result.push_back(std::move(sheet));
        }
        return result;
    }

    void PdmService::addFmeaModule(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        // Add FMEAs
        WiNewPdmNodeQuery newProjectFmeasNodeQuery;
        newProjectFmeasNodeQuery.header = getDefaultPdmProjectFmeasHeader();
        newProjectFmeasNodeQuery.description = getDefaultPdmProjectFmeasDescription();
        newProjectFmeasNodeQuery.parent = semantic;
        newProjectFmeasNodeQuery.role = PdmRoles::ProjectFmeas;
        {
            WiPdmModuleExtension fmeasExtension;
            fmeasExtension.index = 1;
            newProjectFmeasNodeQuery.extension = toJson(fmeasExtension);
        }

        const auto selfSemantic = addNewNodeInternal(initiatingService, newProjectFmeasNodeQuery, sessionPtr, Filter::filterOff, ec, yield, mctx);
        if(ec) return;

        //Add sheets node in FMEAs
        WiNewPdmNodeQuery sheets;
        sheets.header = getDefaultPdmFmeaSheetsHeader();
        sheets.description = getDefaultPdmFmeaSheetsDescription();
        sheets.parent = selfSemantic;
        sheets.role = PdmRoles::FmeaSheets;
        {
            WiPdmModuleExtension sheetsExtension;
            sheetsExtension.index = 2;
            sheets.extension = toJson(std::move(sheetsExtension));
        }
        addNewNodeInternal(initiatingService, sheets, sessionPtr, Filter::filterOff, ec, yield, mctx);

        // Add templates node in FMEAs
        WiNewPdmNodeQuery templates;
        templates.header = getDefaultPdmFmeaTemplatesHeader();
        templates.description = getDefaultPdmFmeaTemplatesDescription();
        templates.parent = selfSemantic;
        templates.role = PdmRoles::FmeaTemplates;
        {
            WiPdmModuleExtension templatesExtension;
            templatesExtension.index = 2;
            templates.extension = toJson(std::move(templatesExtension));
        }
        addNewNodeInternal(initiatingService, templates, sessionPtr, Filter::filterOff, ec, yield, mctx);
    }

    std::optional<WiRbdChain> PdmService::copyRbdChainInternal(
            std::size_t initiatingService,
            const std::string& target_schema_semantic,
            const WiRbdChain &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto start_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr, ec, yield, mctx);
        if (ec || !start_node) return std::nullopt;

        auto end_node = fetchRawNodeEntity(initiatingService,query.target,sessionPtr, ec, yield, mctx);
        if (ec || !end_node) return std::nullopt;

        std::list<WiRbdChain> copies;

        do
        {
            if (start_node->role == PdmRoles::RbdBlock || start_node->role == PdmRoles::SubRbd)
            {
                auto copy_semantic = copyRbdBlockElement(initiatingService, target_schema_semantic,
                                                         start_node->semantic, sessionPtr, ec, yield, mctx);
                if (ec || !copy_semantic.has_value()) return std::nullopt;

                moveRbdRefs(initiatingService, start_node->semantic, copy_semantic->semantic,
                            sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;

                WiRbdLink link;
                link.source = copy_semantic->semantic;
                link.target = copy_semantic->semantic;

                copies.push_back(link);
            }
            else if (start_node->role == PdmRoles::RbdGroupStart)
            {
                auto group = getRbdGroup(initiatingService,start_node->semantic,sessionPtr, ec, yield, mctx);
                if (ec || !group.has_value()) return std::nullopt;

                auto group_copy = copyRbdGroupInternal(initiatingService, target_schema_semantic,
                                                       group.value(), sessionPtr, ec, yield, mctx);
                if (ec || !group_copy.has_value()) return std::nullopt;

                copies.push_back(group_copy.value());

                start_node = fetchRawNodeEntity(initiatingService,group.value().target,sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;
            }

            // end loop condition
            if(start_node->semantic == end_node->semantic)
                break;

            std::optional<std::string> output;
            getRbdElementOutput(initiatingService, start_node, output, sessionPtr, ec, yield, mctx);
            if (ec) return std::nullopt;

            if (!output.has_value()) {
                ec = make_error_code(error::invalid_rbd_element);
                return std::nullopt;
            }

            start_node = fetchRawNodeEntity(initiatingService,output.value(),sessionPtr, ec, yield, mctx);
            if (ec) return std::nullopt;

        } while(true);

        WiRbdChain chain = copies.front();
        for(auto it = ++ copies.begin(); it != copies.end(); ++it)
        {
            WiRbdLink link;
            link.source = chain.target;
            link.target = it->source;
            addRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            chain = *it;
        }

        WiRbdChain result;
        result.source = copies.front().source;
        result.target = copies.back().target;

        return result;
    }

    std::optional<WiRbdChain> PdmService::copyRbdGroupInternal(
                std::size_t initiatingService,
                const std::string& target_schema_semantic,
                const WiRbdChain &orig_group,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto orig_start_node = fetchRawNodeEntity(initiatingService,orig_group.source,sessionPtr,ec,yield,mctx);
        if(ec || !orig_start_node) return std::nullopt;

        if(orig_start_node->role != PdmRoles::RbdGroupStart){
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }

        auto orig_end_node = fetchRawNodeEntity(initiatingService,orig_group.target,sessionPtr,ec,yield,mctx);
        if(ec || !orig_end_node) return std::nullopt;

        if(orig_end_node->role != PdmRoles::RbdGroupEnd){
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }

        checkNode(initiatingService,target_schema_semantic,PdmRoles::RbdSchema,sessionPtr,ec,yield,mctx);
        if (ec) return std::nullopt;

        WiRbdChain result;
        {
            auto group_res = createRbdGroup(initiatingService, target_schema_semantic, sessionPtr, ec, yield, mctx);
            if (!group_res || ec) return std::nullopt;
            result = group_res.value();
        }

        std::vector<std::string> orig_outputs;
        getRbdElementOutputs(initiatingService,orig_start_node,orig_outputs,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        for(const auto& orig_output: orig_outputs)
        {
            WiRbdChain orig_chain;
            orig_chain.source = orig_output;
            orig_chain.target = orig_group.target;

            auto result_chain = copyRbdChainInternal(initiatingService, target_schema_semantic, orig_chain,
                                                     sessionPtr,ec,yield,mctx);
            if (ec) return std::nullopt;

            insertRbdChainIntoGroup(initiatingService, result, result_chain.value(), sessionPtr,ec,yield,mctx);
            if (ec) return std::nullopt;
        }

        return result;
    }

    std::optional<WiSemanticResult> PdmService::copyRbdBlockElement(
                std::size_t initiatingService,
                const std::string& target_schema_semantic,
                const std::string& source_semantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto source_block = fetchRawNodeEntity(initiatingService,source_semantic,sessionPtr, ec, yield, mctx);
        if (ec || !source_block) return std::nullopt;

        if (source_block->role == PdmRoles::RbdBlock)
        {
            WiNewRbdBlock newQuery;
            newQuery.semantic = target_schema_semantic;
            filloutRbdBlockCopyCreationQuery(initiatingService,newQuery,source_semantic,sessionPtr,ec,yield,mctx);

            auto res = addRbdBlock(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;
            if(!res.has_value()) {
                ec = make_error_code(error::invalid_input_data);
                return std::nullopt;
            }
            return res;
        }
        else if(source_block->role == PdmRoles::SubRbd)
        {
            WiAddSubRbdQuery newQuery;
            newQuery.semantic = target_schema_semantic;
            filloutSubRbdCopyCreationQuery(initiatingService,newQuery,source_semantic,sessionPtr,ec,yield,mctx);

            auto res = addSubRbd(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;
            if(!res.has_value()) {
                ec = make_error_code(error::invalid_input_data);
                return std::nullopt;
            }
            return res;
        }
        else
        {
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }
    }

    void PdmService::moveRbdRefs(
            std::size_t initiatingService,
            const std::string& source_rbd_semantic,
            const std::string& target_rbd_semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto sourceRBDNode  = fetchRawNode(initiatingService,source_rbd_semantic,sessionPtr, ec, yield, mctx);
        if(ec || !sourceRBDNode) return;

        auto targetRBDNode  = fetchRawNode(initiatingService,target_rbd_semantic,sessionPtr, ec, yield, mctx);
        if(ec || !targetRBDNode) return;

        if(sourceRBDNode->role != targetRBDNode->role)
        {
            ec = make_error_code(error::invalid_input_data);
            return;
        }

        if (sourceRBDNode->role == PdmRoles::RbdBlock)
        {
            WiPdmRbdBlockExtension RBDBlockExt;
            if(sourceRBDNode->extension.has_value())
            {
                RBDBlockExt = fromJson<WiPdmRbdBlockExtension>(sourceRBDNode->extension.value());
                if (RBDBlockExt.ref.has_value())
                {
                    auto ref = RBDBlockExt.ref.value();
                    unbindPdmComponentWithRbdBlockInternal(initiatingService,
                                                           RBDBlockExt.ref.value(),
                                                           source_rbd_semantic,
                                                           sessionPtr,ec,yield,mctx);
                    if(ec) return;

                    BindPdmComponentWithRbdBlockInternal(initiatingService,
                                                         ref,
                                                         target_rbd_semantic,
                                                         sessionPtr,ec,yield,mctx);
                    if(ec) return;
                }
                else // not a error
                {
                    return;
                }
            };
        }
        else if (sourceRBDNode->role == PdmRoles::SubRbd)
        {
            WiPdmSubRbdExtension subRBDExt;
            if(sourceRBDNode->extension.has_value())
            {
                subRBDExt = fromJson<WiPdmSubRbdExtension>(sourceRBDNode->extension.value());
                if (subRBDExt.ref.has_value())
                {
                    auto ref = subRBDExt.ref.value();
                    unbindRbdSchemaWithSubRbdInternal(initiatingService,
                                                      subRBDExt.ref.value(),
                                                      source_rbd_semantic,
                                                      sessionPtr,ec,yield,mctx);
                    if(ec) return;

                    BindRbdSchemaWithSubRbdInternal(initiatingService,
                                                    ref,
                                                    target_rbd_semantic,
                                                    sessionPtr,ec,yield);
                    if(ec) return;
                }
                else // not a error
                {
                    return;
                }
            };
        }
        else
        {
            ec = make_error_code(error::invalid_node_role);
            return;
        }
    }

    void PdmService::assignRbdLinkSourceOutput(
            std::size_t initiatingService,
            const std::string &source,
            std::optional<std::string> &output,
            const std::string &target,
            const bool overwriteLinks,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,output,target,sessionPtr,ec,yield,mctx);
        if(output.has_value()) {
            if(!overwriteLinks){
                ec = make_error_code(error::rbd_element_output_already_connected);
                return;
            }
            WiRbdLink link;
            link.source = source;
            link.target = output.value();
            removeRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        output = target;
    }

    void PdmService::assignRbdLinkTargetInput(
            std::size_t initiatingService,
            const std::string &source,
            std::optional<std::string> &input,
            const std::string &target,
            const bool overwriteLinks,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,input,target,sessionPtr,ec,yield,mctx);
        if(input.has_value()) {
            if(!overwriteLinks){
                ec = make_error_code(error::rbd_element_input_already_connected);
                return;
            }
            WiRbdLink link;
            link.source = input.value();
            link.target = target;
            removeRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        input = source;
    }

    void PdmService::appendRbdLinkSourceOutputs(
            std::size_t initiatingService,
            const std::string &source,
            std::vector<std::string> &outputs,
            const std::string &target,
            const bool overwriteLinks,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,overwriteLinks,sessionPtr,ec,yield,mctx);
        auto it = std::find_if(outputs.begin(), outputs.end(), [&](auto &&value) {
            return value == target;
        });

        if (outputs.end() == it) {
            outputs.push_back(target);
        }
    }

    void PdmService::appendRbdLinkTargetInputs(
            std::size_t initiatingService,
            const std::string &source,
            std::vector<std::string> &inputs,
            const std::string &target,
            const bool overwriteLinks,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,inputs,target,overwriteLinks,sessionPtr,ec,yield,mctx);
        auto it = std::find_if(inputs.begin(), inputs.end(), [&](auto &&value) {
           return value == source;
       });

        if (inputs.end() == it) {
            inputs.push_back(source);
        }
    }

    void PdmService::insertEdgeElementInsideParallelConnection(
            const std::string &source,
            bool forward,
            const WiRbdChain &chain,
            const WiRbdLink &link,
            std::size_t initiatingService,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept {
        GUARD_PDM_METHOD();

        auto element = fetchRawNodeEntity(initiatingService, source, sessionPtr,ec, yield,mctx);
        if (!element || ec) return;

        auto linkToNode = forward ? fetchRawNode(link.target, ec, yield, mctx) : fetchRawNode(link.source, ec, yield, mctx);
        if (!linkToNode || ec) return;

        if (!linkToNode->extension.has_value())
            return;

        if (PdmRoles::RbdGroupStart == linkToNode->role) {
            auto linkToExtension =  fromJson<WiPdmRbdGroupStartExtension>(linkToNode->extension.value());
            auto &outputs = linkToExtension.outputs;
            std::replace_if(outputs.begin(), outputs.end(), [&element] (const auto &semantic) {return semantic == element->semantic;}, chain.source);

            WiUpdatePdmNodeQuery updateQuery;
            updateQuery.semantic = linkToNode->semantic;
            updateQuery.extension = toJson(linkToExtension);
            updateQuery.updateExtension = true;

            updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return;

            if (element->extension.has_value()) {
                WiUpdatePdmNodeQuery updateQuery;
                updateQuery.semantic = element->semantic;
                updateQuery.updateExtension = true;

                switch(element->role) {
                    case PdmRoles::RbdBlock: {
                        WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.input = chain.target;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    case PdmRoles::SubRbd: {
                        WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.input = chain.target;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    case PdmRoles::RbdGroupStart: {
                        WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.input = chain.target;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    default:
                        ec = make_error_code(error::not_an_rbd_element);
                }

                updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if (ec) return;

                {
                    auto sourceChainNode = fetchRawNode(chain.source, ec, yield, mctx);
                    if (!sourceChainNode || ec) return;

                    nlohmann::json ext;
                    if (sourceChainNode->extension.has_value()) {
                        ext = sourceChainNode->extension.value();
                    }

                    switch (sourceChainNode->role) {
                        case PdmRoles::RbdBlock: {
                            auto extension = fromJson<WiPdmRbdBlockExtension>(ext);
                            extension.input = linkToNode->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        case PdmRoles::RbdGroupStart: {
                            auto extension = fromJson<WiPdmRbdGroupStartExtension>(ext);
                            extension.input = linkToNode->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        default: break;
                    }

                    WiUpdatePdmNodeQuery updateInputChainQuery;
                    updateInputChainQuery.semantic = chain.source;
                    updateInputChainQuery.extension = std::move(ext);
                    updateInputChainQuery.updateExtension = true;

                    updateNode(initiatingService, updateInputChainQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    if (ec) return;
                }
                {
                    auto targetChainNode = fetchRawNode(chain.target, ec, yield, mctx);
                    if (!targetChainNode || ec) return;

                    nlohmann::json ext;
                    if (targetChainNode->extension.has_value()) {
                        ext = targetChainNode->extension.value();
                    }

                    switch (targetChainNode->role) {
                        case PdmRoles::RbdBlock: {
                            auto extension = fromJson<WiPdmRbdBlockExtension>(ext);
                            extension.output = element->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        case PdmRoles::RbdGroupEnd: {
                            auto extension = fromJson<WiPdmRbdGroupEndExtension>(ext);
                            extension.output = element->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        default: break;
                    }

                    WiUpdatePdmNodeQuery updateSourceChainQuery;
                    updateSourceChainQuery.semantic = targetChainNode->semantic;
                    updateSourceChainQuery.extension = std::move(ext);
                    updateSourceChainQuery.updateExtension = true;

                    updateNode(initiatingService, updateSourceChainQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                }
            }
        }
        else if (PdmRoles::RbdGroupEnd == linkToNode->role) {
            auto linkToExtension =  fromJson<WiPdmRbdGroupEndExtension>(linkToNode->extension.value());
            auto &inputs = linkToExtension.inputs;
            std::replace_if(inputs.begin(), inputs.end(), [&element] (const auto &semantic) {return semantic == element->semantic;}, chain.target);

            WiUpdatePdmNodeQuery updateQuery;
            updateQuery.semantic = linkToNode->semantic;
            updateQuery.extension = std::move(toJson(linkToExtension));
            updateQuery.updateExtension = true;

            updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return;

            if (element->extension.has_value()) {
                WiUpdatePdmNodeQuery updateQuery;
                updateQuery.semantic = element->semantic;
                updateQuery.updateExtension = true;

                switch(element->role) {
                    case PdmRoles::RbdBlock: {
                        WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.output = chain.source;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    case PdmRoles::SubRbd: {
                        WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.output = chain.source;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    case PdmRoles::RbdGroupEnd: {
                        WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(element->extension.value());
                        extension.output = chain.source;
                        updateQuery.extension = toJson(extension);
                        break;
                    }
                    default:
                        ec = make_error_code(error::not_an_rbd_element);
                }

                updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if (ec) return;

                {
                    auto sourceChainNode = fetchRawNode(chain.source, ec, yield, mctx);
                    if (!sourceChainNode || ec) return;

                    nlohmann::json ext;
                    if (sourceChainNode->extension.has_value()) {
                        ext = sourceChainNode->extension.value();
                    }

                    switch (sourceChainNode->role) {
                        case PdmRoles::RbdBlock: {
                            auto extension = fromJson<WiPdmRbdBlockExtension>(ext);
                            extension.input = element->semantic;
                            ext = std::move(toJson(extension));
                            break;
                        }
                        case PdmRoles::RbdGroupStart: {
                            auto extension = fromJson<WiPdmRbdGroupStartExtension>(ext);
                            extension.input = element->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        default: break;
                    }

                    WiUpdatePdmNodeQuery updateInputChainQuery;
                    updateInputChainQuery.semantic = chain.source;
                    updateInputChainQuery.extension = std::move(ext);
                    updateInputChainQuery.updateExtension = true;

                    updateNode(initiatingService, updateInputChainQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    if (ec) return;
                }
                {
                    auto targetChainNode = fetchRawNode(chain.target, ec, yield, mctx);
                    if (!targetChainNode || ec) return;

                    nlohmann::json ext;
                    if (targetChainNode->extension.has_value()) {
                        ext = targetChainNode->extension.value();
                    }

                    switch (targetChainNode->role) {
                        case PdmRoles::RbdBlock: {
                            auto extension = fromJson<WiPdmRbdBlockExtension>(ext);
                            extension.output = linkToNode->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        case PdmRoles::RbdGroupEnd: {
                            auto extension = fromJson<WiPdmRbdGroupEndExtension>(ext);
                            extension.output = linkToNode->semantic;
                            ext = toJson(extension);
                            break;
                        }
                        default: break;
                    }

                    WiUpdatePdmNodeQuery updateSourceChainQuery;
                    updateSourceChainQuery.semantic = targetChainNode->semantic;
                    updateSourceChainQuery.extension = std::move(ext);
                    updateSourceChainQuery.updateExtension = true;

                    updateNode(initiatingService, updateSourceChainQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                }
            }
        }
    }

    void PdmService::linkRbdElements(
            std::size_t initiatingService,
            std::shared_ptr<WiPdmRawNodeEntity> source,
            std::shared_ptr<WiPdmRawNodeEntity> target,
            const bool overwriteLinks,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(!source){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!target){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!source->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!target->extension.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        // assign sources outputs
        WiUpdatePdmNodeQuery updateSource(*source);
        switch(source->role){
            case PdmRoles::RbdInputNode: {
                WiPdmRbdInputNodeExtension extension = fromJson<decltype(extension)>(source->extension.value());
                assignRbdLinkSourceOutput(initiatingService, source->semantic, extension.output, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(source->extension.value());
                assignRbdLinkSourceOutput(initiatingService, source->semantic, extension.output, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
                }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(source->extension.value());
                assignRbdLinkSourceOutput(initiatingService, source->semantic, extension.output, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdGroupEnd: {
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(source->extension.value());
                assignRbdLinkSourceOutput(initiatingService, source->semantic, extension.output, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdGroupStart: { // multiple outputs
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(source->extension.value());
                appendRbdLinkSourceOutputs(initiatingService, source->semantic, extension.outputs, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
                }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
        updateSource.updateExtension = true;
        updateNode(initiatingService,updateSource,sessionPtr,Filter::filterOn,ec,yield,mctx);
        if(ec) return;

        // assign targets inputs
        WiUpdatePdmNodeQuery updateTarget(*target);
        switch (target->role) {
            case PdmRoles::RbdOutputNode: {
                WiPdmRbdOutputNodeExtension extension = fromJson<decltype(extension)>(target->extension.value());
                assignRbdLinkTargetInput(initiatingService, source->semantic, extension.input, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                updateTarget.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(target->extension.value());
                assignRbdLinkTargetInput(initiatingService, source->semantic, extension.input, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                updateTarget.extension = toJson(extension);
                }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(target->extension.value());
                assignRbdLinkTargetInput(initiatingService, source->semantic, extension.input, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                if(ec) return;
                updateTarget.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdGroupStart: {
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(target->extension.value());
                assignRbdLinkTargetInput(initiatingService, source->semantic, extension.input, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                updateTarget.extension = toJson(extension);
                }
                break;
            case PdmRoles::RbdGroupEnd: { // multiple inputs
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(target->extension.value());
                appendRbdLinkTargetInputs(initiatingService, source->semantic, extension.inputs, target->semantic, overwriteLinks, sessionPtr, ec, yield, mctx);
                updateTarget.extension = toJson(extension);
                }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
        updateTarget.updateExtension = true;
        updateNode(initiatingService,updateTarget,sessionPtr,Filter::filterOn,ec,yield,mctx);
        if(ec) return;

        return;
    }

    void PdmService::addRbdLink(
            std::size_t initiatingService,
            const WiRbdLink &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto source_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr,ec,yield,mctx);
        if(ec) return;
        auto target_node = fetchRawNodeEntity(initiatingService,query.target,sessionPtr,ec,yield,mctx);
        if(ec) return;
        linkRbdElements(initiatingService,source_node,target_node,false,sessionPtr,ec,yield,mctx);
        return;
    }

    void PdmService::resetRbdLinkSourceOutput(
            std::size_t initiatingService,
            const std::string &source,
            std::optional<std::string> &output,
            const std::string &target,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,target,sessionPtr,ec,yield,mctx);
        if(!output.has_value()) {
            ec = make_error_code(error::rbd_element_output_already_connected);
            return;
        }
        output = std::nullopt;
    }

    void PdmService::resetRbdLinkTargetInput(
            std::size_t initiatingService,
            const std::string &source,
            std::optional<std::string> &input,
            const std::string &target,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,target,sessionPtr,ec,yield,mctx);
        if(!input.has_value()) {
            ec = make_error_code(error::rbd_element_output_already_connected);
            return;
        }
        input = std::nullopt;
    }

    void PdmService::removeRbdLinkSourceOutputs(
            std::size_t initiatingService,
            const std::string &source,
            std::vector<std::string> &outputs,
            const std::string &target,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,target,sessionPtr,ec,yield,mctx);
        auto it = std::find(outputs.begin(),outputs.end(),target);
        if(it== outputs.end()){
            ec = make_error_code(error::rbd_elements_are_not_connected);
            return;
        }
        outputs.erase(it);
    }

    void PdmService::removeRbdLinkTargetInputs(
            std::size_t initiatingService,
            const std::string &source,
            std::vector<std::string> &inputs,
            const std::string &target,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,source,inputs,target,sessionPtr,ec,yield,mctx);
        auto it = std::find(inputs.begin(),inputs.end(),source);
        if(it== inputs.end()){
            ec = make_error_code(error::rbd_elements_are_not_connected);
            return;
        }
        inputs.erase(it);
    }

    void PdmService::unlinkRbdElements(
            std::size_t initiatingService,
            std::shared_ptr<WiPdmRawNodeEntity> source,
            std::shared_ptr<WiPdmRawNodeEntity> target,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(!source){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!target){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!source->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!target->extension.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        // assign sources outputs
        WiUpdatePdmNodeQuery updateSource(*source);
        switch(source->role){
            case PdmRoles::RbdInputNode: {
                WiPdmRbdInputNodeExtension extension = fromJson<decltype(extension)>(source->extension.value());
                resetRbdLinkSourceOutput(
                    initiatingService,
                    source->semantic,
                    extension.output,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(source->extension.value());
                resetRbdLinkSourceOutput(
                    initiatingService,
                    source->semantic,
                    extension.output,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
            }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(source->extension.value());
                resetRbdLinkSourceOutput(
                    initiatingService,
                    source->semantic,
                    extension.output,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdGroupEnd: {
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(source->extension.value());
                resetRbdLinkSourceOutput(
                    initiatingService,
                    source->semantic,
                    extension.output,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdGroupStart: { // multiple outputs
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(source->extension.value());
                removeRbdLinkSourceOutputs(
                    initiatingService,
                    source->semantic,
                    extension.outputs,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(ec) return;
                updateSource.extension = toJson(extension);
            }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
        updateSource.updateExtension = true;
        updateNode(initiatingService,updateSource,sessionPtr,Filter::filterOn,ec,yield,mctx);
        if(ec) return;

        // reset targets inputs
        WiUpdatePdmNodeQuery updateTarget(*target);
        switch(target->role){
            case PdmRoles::RbdOutputNode: {
                WiPdmRbdOutputNodeExtension extension = fromJson<decltype(extension)>(target->extension.value());
                resetRbdLinkTargetInput(
                    initiatingService,
                    source->semantic,
                    extension.input,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                updateTarget.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(target->extension.value());
                resetRbdLinkTargetInput(
                    initiatingService,
                    source->semantic,
                    extension.input,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                updateTarget.extension = toJson(extension);
            }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(target->extension.value());
                resetRbdLinkTargetInput(
                    initiatingService,
                    source->semantic,
                    extension.input,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                updateTarget.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdGroupStart: {
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(target->extension.value());
                resetRbdLinkTargetInput(
                    initiatingService,
                    source->semantic,
                    extension.input,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                updateTarget.extension = toJson(extension);
            }
                break;
            case PdmRoles::RbdGroupEnd: { // multiple inputs
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(target->extension.value());
                removeRbdLinkTargetInputs(
                    initiatingService,
                    source->semantic,
                    extension.inputs,
                    target->semantic,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                updateTarget.extension = toJson(extension);
            }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
        updateTarget.updateExtension = true;
        updateNode(initiatingService,updateTarget,sessionPtr,Filter::filterOn,ec,yield,mctx);
        if(ec) return;

        return;
    }

    void PdmService::removeRbdLink(
            std::size_t initiatingService,
            const WiRbdLink &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto source_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr,ec,yield,mctx);
        if(ec) return;
        auto target_node = fetchRawNodeEntity(initiatingService,query.target,sessionPtr,ec,yield,mctx);
        if(ec) return;
        unlinkRbdElements(initiatingService,source_node,target_node,sessionPtr,ec,yield,mctx);
        return;
    }

    std::optional<WiSemanticResult> PdmService::addRbdBlock(
            std::size_t initiatingService,
            const WiNewRbdBlock &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto rbd = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr,ec,yield,mctx);
        if(ec || !rbd) return std::nullopt;
        if(rbd->role != PdmRoles::RbdSchema){
            ec = make_error_code(error::rbd_elements_cant_be_added_outside_of_rbd);
            return std::nullopt;
        }

        WiPdmRbdBlockExtension rbdBlockExtension;
        WiNewPdmNodeQuery newBlockQuery;
        newBlockQuery.parent = query.semantic;
        newBlockQuery.role = PdmRoles::RbdBlock;
        newBlockQuery.extension = toJson(rbdBlockExtension);
        if (!query.header.has_value() || ! query.description.has_value()){
            // todo: header desc by default
        }else{
            if(query.header.has_value()) {
                newBlockQuery.header = query.header.value();
            }
            if(query.description.has_value()){
                newBlockQuery.description = query.description.value();
            }
        }
        auto selfSemantic = addNewNodeInternal(initiatingService, newBlockQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }
        if(query.ref.has_value()){
            BindPdmComponentWithRbdBlockInternal(initiatingService,query.ref.value(),selfSemantic,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;
        }
        return WiSemanticResult{selfSemantic};
    }

    std::optional<WiSemanticResult> PdmService::addSubRbd(
            std::size_t initiatingService,
            WiAddSubRbdQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto rbd = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr,ec,yield,mctx);
        if(ec || !rbd) return std::nullopt;
        if(rbd->role != PdmRoles::RbdSchema){
            ec = make_error_code(error::rbd_elements_cant_be_added_outside_of_rbd);
            return std::nullopt;
        }

        WiPdmSubRbdExtension SubRbdExtension;
        WiNewPdmNodeQuery newSubRbdQuery;
        newSubRbdQuery.parent = query.semantic;
        newSubRbdQuery.role = PdmRoles::SubRbd;
        newSubRbdQuery.extension = toJson(SubRbdExtension);

        if(query.header.has_value()) {
            newSubRbdQuery.header = query.header.value();
        }
        else
        {
            newSubRbdQuery.header = emptyTranslateText();
        }
        if(query.description.has_value()){
            newSubRbdQuery.description = query.description.value();
        }
        else
        {
            newSubRbdQuery.description = emptyTranslateText();
        }

        auto selfSemantic = addNewNodeInternal(initiatingService, newSubRbdQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }
        if(query.ref.has_value()){
            BindRbdSchemaWithSubRbdInternal(initiatingService,query.ref.value(),selfSemantic,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;
        }
        return WiSemanticResult{selfSemantic};
    }

    void PdmService::deleteRbdBlocks(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::set<std::string> schemas;
        std::set<std::string> elementsForDelete;
        std::set<std::string> openGroup;

        for(const auto &element : query.semantics) {
            const auto verifiableElement = fetchRawNodeEntity(initiatingService, element, sessionPtr, ec, yield, mctx);
            ec = make_error_code(ok);

            switch(verifiableElement->role) {
                case PdmRoles::RbdBlock:
                case PdmRoles::SubRbd: {
                    if(elementsForDelete.find(verifiableElement->semantic) == elementsForDelete.cend()) {
                        elementsForDelete.insert(verifiableElement->semantic);
                    }
                }
                    break;
                case PdmRoles::RbdGroupStart: {
                    WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(verifiableElement->extension.value());
                    if(!extension.end.has_value()){
                        ec = make_error_code(error::rbd_group_is_not_traceable);
                        return;
                    }
                    //Проверяем замыкание группы.
                    if(openGroup.find(extension.end.value()) == openGroup.cend()){
                        openGroup.insert(verifiableElement->semantic);
                        const auto outputs = std::move(extension.outputs);

                        for(const auto &block : outputs){
                            if(elementsForDelete.find(block) == elementsForDelete.cend()) {
                                elementsForDelete.insert(block);
                            }
                        }
                    }else{
                        openGroup.erase(extension.end.value());
                    }
                }
                    break;
                case PdmRoles::RbdGroupEnd: {
                    WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(verifiableElement->extension.value());
                    if(!extension.start.has_value()){
                        ec = make_error_code(error::rbd_group_is_not_traceable);
                        return;
                    }
                    //Проверяем замыкание группы.
                    if(openGroup.find(extension.start.value()) == openGroup.cend()){
                        openGroup.insert(verifiableElement->semantic);
                        const auto inputs = std::move(extension.inputs);

                        for(const auto &block : inputs){
                            if(elementsForDelete.find(block) == elementsForDelete.cend()) {
                                elementsForDelete.insert(block);
                            }
                        }
                    }else{
                        openGroup.erase(extension.start.value());
                    }
                }
                    break;
                default:
                    ec = make_error_code(error::not_an_rbd_element);
            }
            if(ec) return;
        }
        if(!openGroup.empty()){
            ec = make_error_code(error::incorrect_destination);
            return;
        }

        for(const auto &block:elementsForDelete){
            auto schema = nodeNearestAncestor(initiatingService,block,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
            if(ec) return;
            schemas.insert(schema.semantic);
            deleteRbdBlock(initiatingService,WiSemanticOnlyQuery{block},sessionPtr,ec, yield, mctx);

            //если это не блок, то может это свёртка
            if(ec.value() == error::invalid_node_role)
            {
                ec = make_error_code(ok);
                deleteSubRbd(initiatingService,WiSemanticOnlyQuery{block},sessionPtr,ec, yield, mctx);
            }

            if(ec) return;
        }

        //provision schemas
        for(const auto &schema:schemas){
            mctx.addSchemaFlagsTrigger(schema);
            if(ec) return;
        }
    }

    void PdmService::deleteRbdBlock(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService,query.semantic, PdmRoles::RbdBlock,sessionPtr, ec, yield, mctx);
        if(ec) return;

        //проверяем не сопряжён ли блок с элементом ЛСИ
        auto RBDBlockNode  = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !RBDBlockNode) return;

        WiPdmRbdBlockExtension RBDBlockExt;
        if(RBDBlockNode->extension.has_value()){
            RBDBlockExt = fromJson<WiPdmRbdBlockExtension>(RBDBlockNode->extension.value());

            if (RBDBlockExt.ref.has_value())
            {
                unbindPdmComponentWithRbdBlockInternal(
                        initiatingService,
                        RBDBlockExt.ref.value(),
                        query.semantic,
                        sessionPtr,
                        ec,
                        yield,
                        mctx);
                        if(ec) return;
            };

            WiRbdChain chainQuery;
            chainQuery.source = query.semantic;
            chainQuery.target = query.semantic;

            detachRbdChain(initiatingService, chainQuery, true, sessionPtr, ec, yield, mctx);
            if(ec) return;
        };

        deleteNode(initiatingService, query.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    void PdmService::deleteSubRbd(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService,query.semantic, PdmRoles::SubRbd,sessionPtr, ec, yield, mctx);
        if(ec) return;

        //проверяем не сопряжён ли блок с элементом ЛСИ
        auto subRBDNode  = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !subRBDNode) return;

        WiPdmSubRbdExtension subRBDExt;
        if(subRBDNode->extension.has_value()){
            subRBDExt = fromJson<WiPdmSubRbdExtension>(subRBDNode->extension.value());

            if (subRBDExt.ref.has_value())
            {
                unbindRbdSchemaWithSubRbdInternal(
                        initiatingService,
                        subRBDExt.ref.value(),
                        query.semantic,
                        sessionPtr,
                        ec,
                        yield,
                        mctx);
                        if(ec) return;
            };

            WiRbdChain chainQuery;
            chainQuery.source = query.semantic;
            chainQuery.target = query.semantic;

            detachRbdChain(initiatingService, chainQuery, true, sessionPtr, ec, yield, mctx);
            if(ec) return;
        };

        deleteNode(initiatingService, query.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    void PdmService::updateRbdBlockData(
            std::size_t initiatingService,
            const WiUpdateRbdBlockData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto block = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !block) return;

        auto needsUnbinding = [](const std::optional<std::string> &curr, const std::optional<std::string> &changed){
            if(curr.has_value() ) {
                if (changed.has_value()) {
                    if (curr.value() == changed.value()) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        };
        auto needsUpdating = [](const std::optional<std::string> &curr, const std::optional<std::string> &changed){
            if(changed.has_value()){
                if(curr.has_value()){
                    return changed.value() != curr.value();
                }
                return true;
            }
            return false;
        };

        if(block->role == PdmRoles::RbdBlock)
        {
            return updateRbdBlockDataInternal(
                        initiatingService,
                        query,
                        needsUnbinding,
                        needsUpdating,
                        sessionPtr, ec, yield, mctx);
        }
        else if(block->role == PdmRoles::SubRbd)
        {
            return updateSubRbdDataInternal(
                        initiatingService,
                        query,
                        needsUnbinding,
                        needsUpdating,
                        sessionPtr, ec, yield, mctx);
        }
        else
        {
            ec = make_error_code(error::invalid_node_role);
        }
    }

    void PdmService::updateRbdBlockDataInternal(
            std::size_t initiatingService,
            const WiUpdateRbdBlockData &query,
            std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUnbinding,
            std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUpdating,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto block = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !block) return;

        bool refetchNode = false;
        WiPdmRbdBlockExtension extension;
        if(block->extension.has_value())
        {
            extension = fromJson<decltype(extension)>(block->extension.value());
            if(needsUnbinding(extension.ref, query.ref)){
                if(extension.ref.has_value())
                {
                    unbindPdmComponentWithRbdBlockInternal(initiatingService, extension.ref.value(), query.semantic, sessionPtr, ec, yield, mctx);
                    if(ec) return;

                    refetchNode = true;
                }
            }
        }
        if(needsUpdating(extension.ref, query.ref)){
            BindPdmComponentWithRbdBlockInternal(initiatingService,query.ref.value(),query.semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;

            refetchNode = true;
        }
        if(refetchNode){
            block = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
            if(ec || !block) return;
        }

        WiUpdatePdmNodeQuery updateBlock(*block);
        bool updated = false;

        if(query.header.has_value()){
            updateBlock.header = query.header.value();
            updated = true;
        }
        if(query.description.has_value()){
            updateBlock.description = query.description.value();
            updated = true;
        }

        if(updated) {
            updateNode(initiatingService, updateBlock, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return;
        }
    }

    void PdmService::updateSubRbdDataInternal(
            std::size_t initiatingService,
            const WiUpdateRbdBlockData &query,
            std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUnbinding,
            std::function<bool(const std::optional<std::string>&, const std::optional<std::string>&)> needsUpdating,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto block = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !block) return;

        bool refetchNode = false;
        WiPdmSubRbdExtension extension;
        if(block->extension.has_value())
        {
            extension = fromJson<decltype(extension)>(block->extension.value());
            if(needsUnbinding(extension.ref, query.ref)){
                if(extension.ref.has_value())
                {
                    unbindRbdSchemaWithSubRbdInternal(initiatingService, extension.ref.value(), query.semantic, sessionPtr, ec, yield, mctx);
                    if(ec) return;

                    refetchNode = true;
                }
            }
        }
        if(needsUpdating(extension.ref, query.ref)){
            BindRbdSchemaWithSubRbdInternal(initiatingService,query.ref.value(),query.semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;

            refetchNode = true;
        }
        if(refetchNode){
            block = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
            if(ec || !block) return;
        }

        WiUpdatePdmNodeQuery updateBlock(*block);
        bool updated = false;

        if(query.header.has_value()){
            updateBlock.header = query.header.value();
            updated = true;
        }
        if(query.description.has_value()){
            updateBlock.description = query.description.value();
            updated = true;
        }

        if(updated) {
            updateNode(initiatingService, updateBlock, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return;
        }
    }

    void PdmService::getRbdElementInput(
            std::size_t initiatingService,
            const std::shared_ptr<WiPdmRawNodeEntity> &element,
            std::optional<std::string>& input,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(!element){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!element->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        switch(element->role){
            case PdmRoles::RbdOutputNode: {
                WiPdmRbdOutputNodeExtension extension = fromJson<decltype(extension)>(element->extension.value());
                input = extension.input;
            }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(element->extension.value());
                input = extension.input;
            }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(element->extension.value());
                input = extension.input;
            }
                break;
            case PdmRoles::RbdGroupStart: {
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(element->extension.value());
                input = extension.input;
            }
                break;
            case PdmRoles::RbdGroupEnd:  // multiple inputs
                ec = make_error_code(error::rbd_element_has_multiple_inputs);
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;

        return;
    }

    void PdmService::getRbdElementInputs(
            std::size_t initiatingService,
            const std::shared_ptr<WiPdmRawNodeEntity> &element,
            std::vector<std::string>& inputs,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(!element){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!element->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        switch(element->role){
            case PdmRoles::RbdOutputNode:
            case PdmRoles::RbdBlock:
            case PdmRoles::SubRbd:
            case PdmRoles::RbdGroupStart:
                ec = make_error_code(error::rbd_element_has_single_input);
                break;
            case PdmRoles::RbdGroupEnd: {  // multiple inputs
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(element->extension.value());
                inputs = std::move(extension.inputs);
            }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;

        return;
    }

    void PdmService::getRbdElementOutput(
            std::size_t initiatingService,
            const std::shared_ptr<WiPdmRawNodeEntity> &element,
            std::optional<std::string>& output,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(!element){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!element->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        switch(element->role){
            case PdmRoles::RbdInputNode: {
                WiPdmRbdInputNodeExtension extension = fromJson<decltype(extension)>(element->extension.value());
                output = extension.output;
            }
                break;
            case PdmRoles::RbdBlock: {
                WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(element->extension.value());
                output = extension.output;
            }
                break;
            case PdmRoles::SubRbd: {
                WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(element->extension.value());
                output = extension.output;
            }
                break;
            case PdmRoles::RbdGroupEnd: {
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(element->extension.value());
                output = extension.output;
            }
                break;
            case PdmRoles::RbdGroupStart: // multiple outputs
                ec = make_error_code(error::rbd_element_has_multiple_outputs);
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
    }

    void PdmService::getRbdElementOutputs(
            std::size_t initiatingService,
            const std::shared_ptr<WiPdmRawNodeEntity> &element,
            std::vector<std::string>& outputs,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        if(!element){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!element->extension.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        switch(element->role){
            case PdmRoles::RbdInputNode:
            case PdmRoles::RbdBlock:
            case PdmRoles::SubRbd:
            case PdmRoles::RbdGroupEnd:
                ec = make_error_code(error::rbd_element_has_single_output);
                break;
            case PdmRoles::RbdGroupStart:{ // multiple outputs
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(element->extension.value());
                outputs = extension.outputs;
            }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
        }
        if(ec) return;
    }

    void PdmService::validateRbdGroup(
            std::size_t initiatingService,
            const WiRbdChain &group,
            bool validate,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,group,sessionPtr);
        auto start_node = fetchRawNodeEntity(initiatingService,group.source,sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(start_node->role != PdmRoles::RbdGroupStart){
            ec = make_error_code(error::not_an_rbd_group);
            return;
        }
        auto end_node = fetchRawNodeEntity(initiatingService,group.target,sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(start_node->role != PdmRoles::RbdGroupEnd){
            ec = make_error_code(error::not_an_rbd_group);
            return;
        }

        if(!start_node->extension.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!end_node->extension.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        WiPdmRbdGroupStartExtension start_ext = fromJson<decltype(start_ext)>(start_node->extension.value());
        WiPdmRbdGroupEndExtension end_ext = fromJson<decltype(end_ext)>(end_node->extension.value());

        if(!start_ext.end.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        if(!end_ext.start.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        if(end_ext.start.value() != start_node->semantic){
            ec = make_error_code(error::invalid_rbd_group);
            return;
        }
        if(start_ext.end.value() != end_node->semantic){
            ec = make_error_code(error::invalid_rbd_group);
            return;
        }

        if(end_ext.inputs.size() != start_ext.outputs.size()){
            ec = make_error_code(error::rbd_group_is_not_traceable);
            return;
        }

        if(validate && end_ext.inputs.size() == 1){
            ec = make_error_code(error::rbd_group_contains_one_chain_only);
            return;
        }

        // todo: trace all group branches

        return;
    }

    void PdmService::validateRbdChain(
            std::size_t initiatingService,
            const WiRbdChain &chain,
            bool open,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto source = fetchRawNodeEntity(initiatingService,chain.source,sessionPtr, ec,yield,mctx);
        if(!source || ec) return;
        auto target = fetchRawNodeEntity(initiatingService,chain.target,sessionPtr, ec,yield,mctx);
        if(!target || ec) return;
        {
            std::optional<std::string> input, output;
            getRbdElementInput(
                initiatingService,
                source,
                input,
                sessionPtr,
                ec,
                yield,
                mctx);
            if (ec) return;
            getRbdElementOutput(
                initiatingService,
                target,
                output,
                sessionPtr,
                ec,
                yield,
                mctx);
            if (ec) return;
            if (open) {
                if(input.has_value() || output.has_value()){
                    ec = make_error_code(error::rbd_chain_is_not_detached);
                    return;
                }
            }
        }
        auto curr = source;
        while(curr->semantic != target->semantic){
            if (curr->role == PdmRoles::RbdGroupStart){
                auto res = getRbdGroup(initiatingService, curr->semantic, sessionPtr,ec,yield,mctx);
                if(!res || ec) return;
                curr = fetchRawNodeEntity(initiatingService,res.value().target,sessionPtr,ec,yield,mctx);
                if(!curr || ec) return;
                validateRbdGroup(initiatingService,res.value(), false,sessionPtr,ec,yield,mctx);
                if(ec) return;
            }else if(curr->role == PdmRoles::RbdOutputNode) {
                ec = make_error_code(error::invalid_rbd_chain);
                return;
            } else {
                std::optional<std::string> output;
                getRbdElementOutput(initiatingService,curr,output,sessionPtr,ec,yield,mctx);
                if(ec) return;
                if (!output.has_value()){
                    ec = make_error_code(error::invalid_rbd_chain);
                    return;
                }
                curr = fetchRawNodeEntity(initiatingService,output.value(),sessionPtr,ec,yield,mctx);
                if(!curr || ec) return;
            }
        }
        return;
    }

    void PdmService::checkRbdLinkExists(
            std::size_t initiatingService,
            const WiRbdLink &chain,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto source = fetchRawNodeEntity(initiatingService,chain.source,sessionPtr, ec,yield,mctx);
        if(!source || ec) return;
        auto target = fetchRawNodeEntity(initiatingService,chain.target,sessionPtr, ec,yield,mctx);
        if(!source || ec) return;
        switch(source->role){
            case PdmRoles::RbdInputNode:
            case PdmRoles::RbdGroupEnd:
            case PdmRoles::RbdBlock:
            case PdmRoles::SubRbd: {
                std::optional<std::string> output;
                getRbdElementOutput(
                    initiatingService,
                    source,
                    output,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(output.has_value()){
                    if(output.value()!= target->semantic){
                        ec = make_error_code(error::rbd_elements_are_not_connected);
                    }
                }else{
                    ec = make_error_code(error::rbd_elements_are_not_connected);
                }
            }
                break;
            case PdmRoles::RbdGroupStart:{
                std::vector<std::string> outputs;
                getRbdElementOutputs(
                    initiatingService,
                    source,
                    outputs,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                auto it = std::find_if(outputs.begin(), outputs.end(), [&](auto &&value) {
                    return value == target->semantic;
                });

                if(outputs.end() == it){
                    ec = make_error_code(error::rbd_elements_are_not_connected);
                }
            }
                break;
            case PdmRoles::RbdOutputNode:
                ec = make_error_code(error::rbd_output_cant_have_outputs);
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
                break;
        }
        if(ec) return;
        switch(target->role){
            case PdmRoles::RbdOutputNode:
            case PdmRoles::RbdGroupStart:
            case PdmRoles::RbdBlock:
            case PdmRoles::SubRbd: {
                std::optional<std::string> input;
                getRbdElementInput(
                    initiatingService,
                    target,
                    input,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                if(input.has_value()){
                    if(input.value()!= source->semantic){
                        ec = make_error_code(error::rbd_elements_are_not_connected);
                    }
                }else{
                    ec = make_error_code(error::rbd_elements_are_not_connected);
                }
            }
                break;
            case PdmRoles::RbdGroupEnd: {
                std::vector<std::string> inputs;
                getRbdElementInputs(
                    initiatingService,
                    target,
                    inputs,
                    sessionPtr,
                    ec,
                    yield,
                    mctx);
                auto it = std::find_if(inputs.begin(), inputs.end(), [&](auto &&value) {
                    return value == source->semantic;
                });
                if(inputs.end() == it){
                    ec = make_error_code(error::rbd_elements_are_not_connected);
                }
            }
                break;
            case PdmRoles::RbdInputNode:
                ec = make_error_code(error::rbd_input_cant_have_inputs);
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
                break;
        }
        if(ec) return;
    }

    void PdmService::filloutRbdBlockCopyCreationQuery(
            std::size_t initiatingService,
            WiNewRbdBlock &query,
            const std::string &copy,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto copy_block = fetchRawNodeEntity(initiatingService,copy,sessionPtr,ec,yield,mctx);
        if(!copy_block || ec) return;
        if(copy_block->role != PdmRoles::RbdBlock){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        query.header = copy_block->header;
        query.description = copy_block->description;
    }

    void PdmService::filloutSubRbdCopyCreationQuery(
            std::size_t initiatingService,
            WiAddSubRbdQuery &query,
            const std::string &copy,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto copy_block = fetchRawNodeEntity(initiatingService,copy,sessionPtr,ec,yield,mctx);
        if(!copy_block || ec) return;
        if(copy_block->role != PdmRoles::SubRbd){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }
        query.header = copy_block->header;
        query.description = copy_block->description;
    }

    void PdmService::validateRbdBlocksPartQuery(
        const WiNewRbdBlocksPart &query,
        boost::system::error_code &ec) const noexcept(true){
        if(query.elements.has_value()){
            if(query.elements.value().size() > 0) return; // just list of elements
        }
        if(query.count == 0){
            ec = make_error_code(error::invalid_input_data);
            return;
        }
        if(!query.block.has_value()){
            ec = make_error_code(error::invalid_input_data);
            return;
        }
    }

    void PdmService::validateSubsRbdPartQuery(
        const WiNewSubsRbdPart &query,
        boost::system::error_code &ec) const noexcept(true){
        if(query.elements.has_value()){
            if(query.elements.value().size() > 0) return; // just list of elements
        }
        if(query.count == 0){
            ec = make_error_code(error::invalid_input_data);
            return;
        }
        if(!query.sub.has_value()){
            ec = make_error_code(error::invalid_input_data);
            return;
        }
    }

    std::optional<WiRbdChain> PdmService::addRbdBlockChain(
            std::size_t initiatingService,
            const std::string &semantic,
            const WiNewRbdBlocksPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        validateRbdBlocksPartQuery(query,ec);
        if(ec) return std::nullopt;

        auto blocks = addRbdBlocksInternal(initiatingService, semantic, query, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        if(!blocks.has_value()){
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }

        if(blocks.value().semantics.size()<=0){
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }

        WiRbdChain chain;
        chain.source = blocks->semantics.front();
        chain.target = chain.source;
        for(auto it = ++ blocks->semantics.begin(); it != blocks->semantics.end(); ++it){
            WiRbdLink link;
            link.source = chain.target;
            link.target = *it;
            addRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            chain.target = std::move(*it);
        }
        return chain;
    }

    std::optional<WiRbdChain> PdmService::addSubRbdChain(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        validateSubsRbdPartQuery(query,ec);
        if(ec) return std::nullopt;

        auto blocks = addSubsRbdInternal(initiatingService, semantic, query, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        if(!blocks.has_value()){
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }

        if(blocks.value().semantics.size()<=0){
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }

        WiRbdChain chain;
        chain.source = blocks->semantics.front();
        chain.target = chain.source;
        for(auto it = ++ blocks->semantics.begin(); it != blocks->semantics.end(); ++it){
            WiRbdLink link;
            link.source = chain.target;
            link.target = *it;
            addRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            chain.target = std::move(*it);
        }
        return chain;
    }

    void PdmService::addRbdBlocksToGroup(
            std::size_t initiatingService,
            const WiRbdChain &group,
            const WiNewRbdBlocksPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        validateRbdBlocksPartQuery(query,ec);
        if(ec) return;

        std::string rbd;
        {
            auto rbd_node = nodeNearestAncestor(initiatingService,group.source,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
            if(ec) return;
            rbd = rbd_node.semantic;
        }

        auto blocks = addRbdBlocksInternal(initiatingService, rbd, query, sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(!blocks.has_value()) {
            ec = make_error_code(error::invalid_input_data);
            return;
        }

        for(const auto &block: blocks->semantics){
            WiRbdLink groupToBlock;
            groupToBlock.source = group.source;
            groupToBlock.target = block;
            addRbdLink(initiatingService,groupToBlock,sessionPtr,ec,yield,mctx);
            if(ec) return;

            WiRbdLink blockToGroup;
            blockToGroup.source = block;
            blockToGroup.target = group.target;
            addRbdLink(initiatingService,blockToGroup,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        return;
    }

    void PdmService::addSubsRbdToGroup(
            std::size_t initiatingService,
            const WiRbdChain &group,
            const WiNewSubsRbdPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        validateSubsRbdPartQuery(query,ec);
        if(ec) return;

        std::string rbd;
        {
            auto rbd_node = nodeNearestAncestor(initiatingService,group.source,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
            if(ec) return;
            rbd = rbd_node.semantic;
        }

        auto blocks = addSubsRbdInternal(initiatingService, rbd, query, sessionPtr,ec,yield,mctx);
        if(ec) return;
        if(!blocks.has_value()) {
            ec = make_error_code(error::invalid_input_data);
            return;
        }

        for(const auto &block: blocks->semantics){
            WiRbdLink groupToBlock;
            groupToBlock.source = group.source;
            groupToBlock.target = block;
            addRbdLink(initiatingService,groupToBlock,sessionPtr,ec,yield,mctx);
            if(ec) return;

            WiRbdLink blockToGroup;
            blockToGroup.source = block;
            blockToGroup.target = group.target;
            addRbdLink(initiatingService,blockToGroup,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
        return;
    }

    std::optional<WiRbdChain> PdmService::addRbdBlockGroup(
            std::size_t initiatingService,
            const std::string &semantic,
            const WiNewRbdBlocksPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService, semantic,PdmRoles::RbdSchema,sessionPtr,ec,yield,mctx);
        if (ec) return std::nullopt;

        // add group, create blocks and link them to group
        WiRbdChain group;
        {
            auto group_res = createRbdGroup(initiatingService, semantic, sessionPtr, ec, yield, mctx);
            if (!group_res || ec) return std::nullopt;
            group = group_res.value();
        }

        addRbdBlocksToGroup(initiatingService,group,query,sessionPtr,ec,yield,mctx);
        if (ec) return std::nullopt;

        return group;
    }


    std::optional<WiRbdChain> PdmService::addSubRbdGroup(
            std::size_t initiatingService,
            const std::string &semantic,
            const WiNewSubsRbdPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode( initiatingService,semantic,PdmRoles::RbdSchema,sessionPtr,ec,yield,mctx);
        if (ec) return std::nullopt;

        // add group, create blocks and link them to group
        WiRbdChain group;
        {
            auto group_res = createRbdGroup(initiatingService, semantic, sessionPtr, ec, yield, mctx);
            if (!group_res || ec) return std::nullopt;
            group = group_res.value();
        }

        addSubsRbdToGroup(initiatingService,group,query,sessionPtr,ec,yield,mctx);
        if (ec) return std::nullopt;

        return group;
    }

    std::optional<WiRbdChain> PdmService::addRbdBlocks(
            std::size_t initiatingService,
            const WiNewRbdBlocks &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.parallel) {
            return addRbdBlockGroup(initiatingService, query.semantic, query.blocks, sessionPtr, ec, yield, mctx);
        }
        return addRbdBlockChain(initiatingService, query.semantic, query.blocks,sessionPtr, ec, yield, mctx);
    }

    std::optional<WiRbdChain> PdmService::addSubsRbd(
                std::size_t initiatingService,
                const WiNewSubsRbd &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        if(query.parallel) {
            return addSubRbdGroup(initiatingService, query.semantic, query.subs, sessionPtr, ec, yield, mctx);
        }
        return addSubRbdChain(initiatingService, query.semantic, query.subs,sessionPtr, ec, yield, mctx);
    }

    std::optional<WiSemanticsResult> PdmService::addRbdBlocksInternal(
            std::size_t initiatingService,
            const std::string &semantic,
            const WiNewRbdBlocksPart &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        WiSemanticsResult ret;
        if(query.block.has_value() && query.count > 0){
            for(std::uint64_t i = 0; i < query.count; ++i){
                WiNewRbdBlock newQuery;
                newQuery.semantic = semantic;
                newQuery.header = query.block->header;
                newQuery.description = query.block->description;
                auto res = addRbdBlock(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
                if(ec) return std::nullopt;
                if(!res.has_value()) {
                    ec = make_error_code(error::invalid_input_data);
                    return std::nullopt;
                }
                ret.semantics.push_back(std::move(res->semantic));
            }
        }else if(query.elements.has_value()){
            for(const auto &element : query.elements.value()){
                WiNewRbdBlock newQuery;
                newQuery.semantic = semantic;
                newQuery.ref = element;
                auto res = addRbdBlock(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
                if(ec) return std::nullopt;
                if(!res.has_value()) {
                    ec = make_error_code(error::invalid_input_data);
                    return std::nullopt;
                }
                ret.semantics.push_back(std::move(res->semantic));
            }
        }else{
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }
        mctx.addSchemaFlagsTrigger(semantic);
        if(ec) return std::nullopt;
        return ret;
    }

    void PdmService::validateInsertRbdBesideQuery(
            const WiRbdInsertBeside &query,
            boost::system::error_code &ec) const noexcept(true){
        //check that only one of the args set
        if( (!query.chain.has_value()&&!query.blocks.has_value()&&!query.subs.has_value())
            || (query.chain.has_value()&&query.blocks.has_value())
            || (query.chain.has_value()&&query.subs.has_value())
            || (query.blocks.has_value()&&query.subs.has_value())){
            ec= make_error_code(error::invalid_input_data);
            return;
        }
        if(query.blocks.has_value()){
            if(!query.parallel.has_value())
            {
                ec = make_error_code(error::invalid_input_data);
                return;
            }

            if (query.parallel.value() == true && query.blocks.value().count < 2)
            {
                ec = make_error_code(error::invalid_input_data);
                return;
            }
        }
        if(query.subs.has_value()){
            if(!query.parallel.has_value())
            {
                ec = make_error_code(error::invalid_input_data);
                return;
            }

            if (query.parallel.value() == true && query.subs.value().count < 2)
            {
                ec = make_error_code(error::invalid_input_data);
                return;
            }
        }
    }

    std::optional<WiSemanticsResult> PdmService::addSubsRbdInternal(
                std::size_t initiatingService,
                const std::string &semantic,
                const WiNewSubsRbdPart &query,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const net::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
         GUARD_PDM_METHOD();
        WiSemanticsResult ret;
        if(query.sub.has_value() && query.count > 0){
            for(std::uint64_t i = 0; i < query.count; ++i){
                WiAddSubRbdQuery newQuery;
                newQuery.semantic = semantic;
                newQuery.header = query.sub->header;
                newQuery.description = query.sub->description;
                auto res = addSubRbd(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
                if(ec) return std::nullopt;
                if(!res.has_value()) {
                    ec = make_error_code(error::invalid_input_data);
                    return std::nullopt;
                }
                ret.semantics.push_back(std::move(res->semantic));
            }
        }else if(query.elements.has_value()){
            for(const auto &element : query.elements.value()){
                WiAddSubRbdQuery newQuery;
                newQuery.semantic = semantic;
                newQuery.ref = element;
                auto res = addSubRbd(initiatingService,newQuery,sessionPtr,ec,yield,mctx);
                if(ec) return std::nullopt;
                if(!res.has_value()) {
                    ec = make_error_code(error::invalid_input_data);
                    return std::nullopt;
                }
                ret.semantics.push_back(std::move(res->semantic));
            }
        }else{
            ec = make_error_code(error::invalid_input_data);
            return std::nullopt;
        }
        mctx.addSchemaFlagsTrigger(semantic);
        if(ec) return std::nullopt;
        return ret;
    }

    void PdmService::insertRbdBetween(
            std::size_t initiatingService,
            const WiRbdLink &link,
            const WiRbdChain &chain,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr,mctx);
        checkRbdLinkExists(initiatingService,link,sessionPtr,ec,yield,mctx);
        if(ec) return;
        //remove old link
        removeRbdLink(initiatingService,link,sessionPtr,ec,yield,mctx);
        if(ec) return;

        //create new link comm source with chain start
        WiRbdLink startLink;
        startLink.source = link.source;
        startLink.target = chain.source;
        addRbdLink(initiatingService,startLink,sessionPtr,ec,yield,mctx);
        if(ec) return;

        //create new link comm chain end with target
        WiRbdLink endLink;
        endLink.source = chain.target;
        endLink.target = link.target;
        addRbdLink(initiatingService,endLink,sessionPtr,ec,yield,mctx);
        if(ec) return;
    }

    void PdmService::insertRbdBeside(
            std::size_t initiatingService,
            const WiRbdInsertBeside &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        validateInsertRbdBesideQuery(query,ec);
        if (ec) return;

        auto element = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr,ec,yield,mctx);
        if (ec) return;

        std::optional<std::string> linked_to;
        if (query.forward) {
            getRbdElementOutput(initiatingService, element,linked_to, sessionPtr,ec, yield,mctx);
        } else {
            getRbdElementInput(initiatingService, element,linked_to, sessionPtr,ec, yield,mctx);
        }

        if (ec) return;

        WiRbdChain chain;
        if (query.chain.has_value()) {
            validateRbdChain(initiatingService,query.chain.value(),true,sessionPtr,ec,yield,mctx);
            if (ec) return;
            chain = query.chain.value();
        } else if (query.blocks.has_value()) {
            auto rbd = nodeNearestAncestor(initiatingService, query.semantic, PdmRoles::RbdSchema, 1, sessionPtr, ec, yield, mctx);
            if (ec) return;

            WiNewRbdBlocks blocksQuery;
            blocksQuery.semantic = rbd.semantic;
            blocksQuery.blocks = query.blocks.value();
            blocksQuery.parallel = false;
            if (query.parallel.has_value()) {
                blocksQuery.parallel = query.parallel.value();
            }

            auto res = addRbdBlocks(initiatingService, blocksQuery, sessionPtr, ec, yield, mctx);
            if (!res.has_value() || ec) return;

            chain = res.value();
        } else if (query.subs.has_value()) {
            auto rbd = nodeNearestAncestor(initiatingService, query.semantic, PdmRoles::RbdSchema, 1, sessionPtr, ec, yield, mctx);
            if (ec) return;

            WiNewSubsRbd subsQuery;
            subsQuery.semantic = rbd.semantic;
            subsQuery.subs = query.subs.value();
            subsQuery.parallel = false;
            if (query.parallel.has_value())
                subsQuery.parallel = query.parallel.value();

            auto res = addSubsRbd(initiatingService, subsQuery, sessionPtr, ec, yield, mctx);
            if (!res.has_value() || ec) return;

            chain = res.value();
        } else {
            ec = make_error_code(error::invalid_input_data);
            return;
        }

        if (linked_to.has_value()) {
            WiRbdLink link;
            if (query.forward) {
                link.source = query.semantic;
                link.target = linked_to.value();
            } else {
                link.source = linked_to.value();
                link.target = query.semantic;
            }

            auto linkToNode = query.forward ? fetchRawNode(link.target, ec, yield, mctx) : fetchRawNode(link.source, ec, yield, mctx);
            if (ec) return;
            auto targetNode = query.forward ? fetchRawNode(link.source, ec, yield, mctx) : fetchRawNode(link.target, ec, yield, mctx);
            if (ec) return;

            if ((query.forward && PdmRoles::RbdGroupStart == linkToNode->role)
                    ||(!query.forward && PdmRoles::RbdGroupEnd == linkToNode->role)
                    ||(PdmRoles::RbdBlock == linkToNode->role && PdmRoles::RbdGroupEnd == targetNode->role)
                    ||(PdmRoles::RbdBlock == linkToNode->role && PdmRoles::RbdGroupStart == targetNode->role)
                    ||(PdmRoles::RbdBlock == linkToNode->role && PdmRoles::RbdBlock == targetNode->role)
                    ||(PdmRoles::RbdGroupEnd == linkToNode->role && PdmRoles::RbdBlock == targetNode->role)) {
                insertRbdBetween(initiatingService, link, chain, sessionPtr,ec, yield,mctx);
                return;
            }
            if ((PdmRoles::RbdInputNode == targetNode->role || PdmRoles::RbdOutputNode == targetNode->role) //Если это вход/выход схемы
                    ||(PdmRoles::RbdInputNode == linkToNode->role || PdmRoles::RbdOutputNode == linkToNode->role)
                    ||(PdmRoles::RbdGroupStart == linkToNode->role && PdmRoles::RbdGroupEnd == targetNode->role)  //Если выставляем между узлами групп
                    ||(PdmRoles::RbdGroupEnd == linkToNode->role && PdmRoles::RbdGroupStart == targetNode->role)) {     //Если выставляем между узлами групп
                insertRbdBetween(initiatingService, link, chain, sessionPtr,ec, yield,mctx);
            } else if ((PdmRoles::RbdGroupStart == linkToNode->role || PdmRoles::RbdGroupEnd == linkToNode->role)
                    && linkToNode->extension.has_value()) {
                insertEdgeElementInsideParallelConnection(query.semantic, query.forward, chain, link, initiatingService, sessionPtr, ec, yield, mctx);
            }
            else {
                ec = make_error_code(error::invalid_input_data);
            }
        } else {
            WiRbdLink link;
            if (query.forward) {
                link.source = query.semantic;
                link.target = chain.source;
            } else {
                link.source = chain.target;
                link.target = query.semantic;
            }

            addRbdLink(initiatingService, link, sessionPtr, ec, yield, mctx);
            if(ec) return;
        }
    }

    void PdmService::validateInsertRbdInParallelQuery(
            const WiRbdInsertInParallel &query,
            boost::system::error_code &ec) const noexcept(true){
        if( (!query.chain.has_value()&&!query.blocks.has_value()&&!query.subs.has_value())
            || (query.chain.has_value()&&query.blocks.has_value())
            || (query.chain.has_value()&&query.subs.has_value())
            || (query.blocks.has_value()&&query.subs.has_value())){
            ec = make_error_code(error::invalid_input_data);
        }
    }

    void PdmService::insertRbdIntoParallel(
            std::size_t initiatingService,
            const WiInsertRdbIntoParallel &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        // Проверяем, что запрос пришел от блока
        checkNode(initiatingService, query.semantic, PdmRoles::RbdBlock, sessionPtr,  ec, yield, mctx);
        if (ec) return;

        // Нода относительно которой будем вставлять узлы
        auto currentElement = fetchRawNodeEntity( initiatingService, query.semantic, sessionPtr,ec,yield,mctx);
        if (!currentElement->extension.has_value()) {
            ec = make_error_code(invalid_rbd_element);
            return;
        }

        // Берем ее расширение
        auto extension = fromJson<WiPdmRbdBlockExtension>(currentElement->extension.value());
        if(!extension.input.has_value()) {
            ec = make_error_code(invalid_rbd_element);
            return;
        }
        std::string outputSemanticToGroup = currentElement->semantic;
        std::shared_ptr<WiPdmRawNodeEntity> leftNode = fetchRawNodeEntity( initiatingService, extension.input.value(), sessionPtr,ec,yield,mctx);
        if (nullptr == leftNode) {
            ec = make_error_code(invalid_rbd_element);
            return;
        }
        //Ищем левый крайний в группе
        while (PdmRoles::RbdGroupStart != leftNode->role) {
            if (!leftNode->extension.has_value()) {
                ec = make_error_code(invalid_rbd_element);
                return;
            }
            if(PdmRoles::RbdGroupEnd == leftNode->role) {
               auto extensionNode = fromJson<WiPdmRbdGroupEndExtension>(leftNode->extension.value());
               if (extensionNode.start.has_value()) {
                   ec = make_error_code(invalid_rbd_element);
                   return;
               }
               std::shared_ptr<WiPdmRawNodeEntity> startNodeSubGroup = fetchRawNodeEntity( initiatingService, extensionNode.start.value(), sessionPtr,ec,yield,mctx);

               const auto extensionSubGroupNode = fromJson<WiPdmRbdGroupStartExtension>(startNodeSubGroup->extension.value());
               if (extensionSubGroupNode.input.has_value()) {
                   ec = make_error_code(invalid_rbd_element);
                   return;
               }

               outputSemanticToGroup = startNodeSubGroup->semantic;
               leftNode = fetchRawNodeEntity( initiatingService, extensionSubGroupNode.input.value(), sessionPtr,ec,yield,mctx);
               continue;
            }
            if (PdmRoles::RbdBlock != leftNode->role) {
                ec = make_error_code(invalid_rbd_element);
                return;
            }
            const auto currentNodeExtension = fromJson<WiPdmRbdBlockExtension>(leftNode->extension.value());
            outputSemanticToGroup = leftNode->semantic;
            leftNode = fetchRawNodeEntity( initiatingService, currentNodeExtension.input.value(), sessionPtr,ec,yield,mctx);
        }

        if(!extension.output.has_value()) {
            ec = make_error_code(invalid_rbd_element);
            return;
        }

        std::string inputSemanticToGroup = currentElement->semantic;
        std::shared_ptr<WiPdmRawNodeEntity> rightNode = fetchRawNodeEntity( initiatingService, extension.output.value(), sessionPtr,ec,yield,mctx);

        if (nullptr == rightNode) {
            ec = make_error_code(invalid_rbd_element);
            return;
        }
        //Ищем правый крайний в группе
        while (PdmRoles::RbdGroupEnd != rightNode->role) {
            if (!rightNode->extension.has_value()) {
                ec = make_error_code(invalid_rbd_element);
                return;
            }
            if(PdmRoles::RbdGroupStart == rightNode->role){
                auto extensionNode = fromJson<WiPdmRbdGroupStartExtension>(rightNode->extension.value());
                if (extensionNode.end.has_value()) {
                    ec = make_error_code(invalid_rbd_element);
                    return;
                }
                std::shared_ptr<WiPdmRawNodeEntity> endNodeSubGroup = fetchRawNodeEntity( initiatingService, extensionNode.end.value(), sessionPtr,ec,yield,mctx);
                const auto extensionSubGroupNode = fromJson<WiPdmRbdGroupEndExtension>(endNodeSubGroup->extension.value());
                if (extensionSubGroupNode.output.has_value()) {
                    ec = make_error_code(invalid_rbd_element);
                    return;
                }

                inputSemanticToGroup = endNodeSubGroup->semantic;
                rightNode = fetchRawNodeEntity( initiatingService, extensionSubGroupNode.output.value(), sessionPtr,ec,yield,mctx);
                continue;
            }
            if (PdmRoles::RbdBlock != rightNode->role) {
                ec = make_error_code(invalid_rbd_element);
                return;
            }
            const auto currentNodeExtension = fromJson<WiPdmRbdBlockExtension>(rightNode->extension.value());
            inputSemanticToGroup = rightNode->semantic;
            rightNode = fetchRawNodeEntity( initiatingService, currentNodeExtension.output.value(), sessionPtr,ec,yield,mctx);
        }

        auto rbdNode = nodeNearestAncestor(initiatingService, query.semantic,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);

        auto leftNodeExtension = fromJson<WiPdmRbdGroupStartExtension>(leftNode->extension.value());
        auto rightNodeExtension = fromJson<WiPdmRbdGroupEndExtension>(rightNode->extension.value());

        auto leftNodeInsertIterator = std::find_if(
            leftNodeExtension.outputs.begin(),
            leftNodeExtension.outputs.end(), [&](auto &&value) {
            return value == outputSemanticToGroup;
        });

        auto rightInsertIterator = std::find_if(
            rightNodeExtension.inputs.begin(),
            rightNodeExtension.inputs.end(),
            [&](auto &&value) {
            return value == inputSemanticToGroup;
        });
        if (!query.position) {
            leftNodeInsertIterator = ++leftNodeInsertIterator;
            rightInsertIterator = ++ rightInsertIterator;
        }

        for (std::uint64_t i = 0; i < query.blocks.count; i++) {
            WiPdmRbdBlockExtension rbdBlockExtension;
            rbdBlockExtension.input = leftNode->semantic;
            rbdBlockExtension.output = rightNode->semantic;
            WiNewPdmNodeQuery newBlockQuery;
            newBlockQuery.parent = rbdNode.semantic;
            newBlockQuery.role = PdmRoles::RbdBlock;
            newBlockQuery.extension = toJson(rbdBlockExtension);
            // todo header - description
            auto selfSemantic = addNewNodeInternal(initiatingService, newBlockQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);

            leftNodeInsertIterator = leftNodeExtension.outputs.insert(leftNodeInsertIterator, selfSemantic);
            rightInsertIterator = rightNodeExtension.inputs.insert(rightInsertIterator, selfSemantic);
        }
        WiUpdatePdmNodeQuery update_left_node_query(*leftNode);
        update_left_node_query.extension = toJson(leftNodeExtension);
        update_left_node_query.updateExtension = true;
        updateNode(initiatingService, update_left_node_query, sessionPtr, Filter::filterOn, ec, yield, mctx);

        WiUpdatePdmNodeQuery update_right_node_query(*rightNode);
        update_right_node_query.extension = toJson(rightNodeExtension);
        update_right_node_query.updateExtension = true;
        updateNode(initiatingService, update_right_node_query, sessionPtr, Filter::filterOn, ec, yield, mctx);
        mctx.addSchemaTrigger(rbdNode.semantic);
        mctx.addSchemaFlagsTrigger(rbdNode.semantic);
    }

    void PdmService::changeElementFailureTypes(
        std::size_t initiatingService,
        const std::string &elementSemantic,
        const WiFailureTypeQuery &query,
        const std::shared_ptr<IWiSession> sessionPtr,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        const auto element = fetchRawNodeEntity(initiatingService, elementSemantic, sessionPtr, ec, yield, mctx);
        if (ec) return;

        auto project = nodeNearestAncestor(initiatingService, elementSemantic, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if (ec) return;

        auto dictionaries = nodeNearestDescendant(initiatingService, project.semantic, PdmRoles::Dictionaries, 1, sessionPtr, ec, yield, mctx);
        std::string dictionariesSemantic;

        if (ec) {
            ec = make_error_code(boost::system::errc::success);
            WiNewPdmNodeQuery newDictionariesNodeQuery;
            newDictionariesNodeQuery.role = PdmRoles::Dictionaries;
            newDictionariesNodeQuery.parent = project.semantic;
            dictionariesSemantic = addNewNodeInternal(initiatingService, newDictionariesNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) {
                return;
            }
        }
        else {
            dictionariesSemantic = dictionaries.semantic;
        }

        std::string failureTypeDictionarySemantic;
        auto failureTypeDictionary = nodeNearestDescendant(initiatingService, dictionariesSemantic, PdmRoles::FailureTypeDictionary, 1, sessionPtr, ec, yield, mctx);

        if (ec) {
            ec = make_error_code(boost::system::errc::success);
            WiNewPdmNodeQuery newFailureTypeDictionaryNodeQuery;
            newFailureTypeDictionaryNodeQuery.role = PdmRoles::FailureTypeDictionary;
            newFailureTypeDictionaryNodeQuery.parent = dictionariesSemantic;
            failureTypeDictionarySemantic = addNewNodeInternal(initiatingService, newFailureTypeDictionaryNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) {
                return;
            }
        }
        else {
            failureTypeDictionarySemantic = failureTypeDictionary.semantic;
        }

        auto &failureTypesCache = PlainCache.getEntitiesCache<WiPdmFailureType>();

        WiPdmElementData elementData;
        if (element->entity.has_value()) {
            const auto componentEntity = element->entity.value();
            if (componentEntity.data.has_value())
                elementData = fromJson<WiPdmElementData>(componentEntity.data.value());
        }

        std::vector<WiPdmElementFailureType> elementFailureTypes;
        if (elementData.failure_types.has_value())
            elementFailureTypes = elementData.failure_types.value();

        // Обработка удаленных типов отказа
        if (query.removedFailureTypes.has_value()) {
            for (const auto &semantic : query.removedFailureTypes.value()) {
                //  обновляем список ссылок типа отказа на элементы и удаляем тип отказа если ссылок не осталось
                auto entityToUpdate = failureTypesCache.getEntityBySemantic(semantic);
                if (entityToUpdate.isEmpty())
                    continue;

                for (auto it = elementFailureTypes.begin(); it != elementFailureTypes.end(); ++it) {
                    if (semantic == it->semantic) {
                        elementFailureTypes.erase(it);
                        break;
                    }
                }

                auto &refs = entityToUpdate.refs;
                auto it = std::find(refs.begin(), refs.end(), element->semantic);
                if (it != refs.end()) {
                    std::swap(*it, refs.back());
                    refs.pop_back();
                }

                // Просто удаляем вид отказа
                if (refs.empty()) {
                    failureTypesCache.removeEntity(semantic, [&] (auto &&entity) {
                        deleteNode(initiatingService, entity.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    });

                    continue;
                }

                // Обновляем ссылки виду отказа
                failureTypesCache.updateEntity(entityToUpdate, [&] (auto &&entity) {
                    WiPdmDictionaryData data;
                    data.refs = entity.refs;

                    WiUpdatePdmNodeQuery updateFailureTypeQuery;
                    updateFailureTypeQuery.data = toJson(data);
                    updateFailureTypeQuery.updateData = true;
                    updateFailureTypeQuery.semantic = entity.semantic;
                    updateNode(initiatingService, updateFailureTypeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                });
            }
        }

        // Обработка измененных типов отказа
        if (query.changedFailureTypes.has_value()) {
            for (const auto &changedFailureType : query.changedFailureTypes.value()) {
                // Проверяем есть ли данный вид отказа
                auto entityToUpdate = failureTypesCache.getEntityBySemantic(changedFailureType.semantic);
                if (entityToUpdate.isEmpty())
                    continue;

                // Добавляем ссылку на элемент, если ссылки не было ранее
                auto &elementRefs = entityToUpdate.refs;
                if (auto refIt = std::find_if(elementRefs.begin(), elementRefs.end(), [&elementSemantic](const auto &ref) {return elementSemantic == ref;}); refIt == elementRefs.end())
                    elementRefs.push_back(elementSemantic);

                // Если сть наименование - обновляем наименование
                if (changedFailureType.header.has_value()) {
                    entityToUpdate.header = getTranslatedValue(changedFailureType.header.value(), m_defaultLanguage);
                }

                failureTypesCache.updateEntity(entityToUpdate, [&] (auto &&entity) {
                    WiUpdatePdmNodeQuery updateFailureTypeQuery;
                    WiPdmDictionaryData data;
                    data.refs = entity.refs;
                    if (changedFailureType.header.has_value())
                        updateFailureTypeQuery.header = changedFailureType.header.value();

                    updateFailureTypeQuery.semantic = changedFailureType.semantic;
                    updateFailureTypeQuery.data = toJson(data);
                    updateFailureTypeQuery.updateData = true;
                    updateNode(initiatingService, updateFailureTypeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                });

                auto it = std::find_if(elementFailureTypes.begin(), elementFailureTypes.end(), [&entityToUpdate](const WiPdmElementFailureType &failureType) {
                    return failureType.semantic == entityToUpdate.semantic;
                });

                if (it != elementFailureTypes.end()) {
                    it->proportion = changedFailureType.proportion;
                }
                else {
                    elementFailureTypes.push_back(WiPdmElementFailureType{entityToUpdate.semantic, changedFailureType.proportion});
                }
            }
        }

        // Обработка добавленных типов отказа
        if (query.addedFailureTypes.has_value()) {
            for (const auto &addedFailureType : query.addedFailureTypes.value()) {
                // Наименование обязательно
                if (!addedFailureType.header.has_value()) continue;

                WiPdmFailureType newFailureType;
                newFailureType.projectSemantic = project.semantic;
                newFailureType.header = getTranslatedValue(addedFailureType.header.value(), m_defaultLanguage);
                newFailureType.refs = {elementSemantic};

                failureTypesCache.addEntity(newFailureType, [&] (auto &&entity) {
                    WiNewPdmNodeQuery newFailureTypeNodeQuery;
                    newFailureTypeNodeQuery.role = PdmRoles::FailureType;
                    newFailureTypeNodeQuery.parent = failureTypeDictionarySemantic;
                    newFailureTypeNodeQuery.header = addedFailureType.header.value();

                    WiPdmDictionaryData data;
                    data.refs = {elementSemantic};
                    newFailureTypeNodeQuery.data = toJson(data);

                    auto selfSemantic = addNewNodeInternal(initiatingService, newFailureTypeNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    if (ec) return;

                    entity.semantic = selfSemantic;
                    elementFailureTypes.push_back(WiPdmElementFailureType{selfSemantic, addedFailureType.proportion});
                });
            }
        }

        elementData.failure_types = elementFailureTypes;
        WiUpdatePdmNodeQuery updateElementQuery;
        updateElementQuery.semantic = element->semantic;
        updateElementQuery.data = toJson(elementData);
        updateElementQuery.updateData = true;

        updateNode(initiatingService, updateElementQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    void PdmService::removeElementFailureTypes(
            std::size_t initiatingService,
            const std::string &elementSemantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        const auto element = fetchRawNodeEntity(initiatingService, elementSemantic, sessionPtr, ec, yield, mctx);
        if (!element || ec) return;

        WiPdmElementData elementData;
        if (element->entity.has_value()) {
            const auto componentEntity = element->entity.value();
            if (componentEntity.data.has_value())
                elementData = fromJson<WiPdmElementData>(componentEntity.data.value());
        }
        else {
            return;
        }

        std::vector<WiPdmElementFailureType> elementFailureTypes;
        if (elementData.failure_types.has_value()) {
            elementFailureTypes = elementData.failure_types.value();
        }
        else {
            return;
        }

        std::vector<std::string> failureTypesToDelete;
        failureTypesToDelete.reserve(elementFailureTypes.size());
        for (const auto& failureType : elementFailureTypes) {
            failureTypesToDelete.emplace_back(std::move(failureType.semantic));
        }

        auto &failureTypesCache = PlainCache.getEntitiesCache<WiPdmFailureType>();

        for (const auto &semantic : failureTypesToDelete) {
            //  обновляем список ссылок типа отказа на элементы и удаляем тип отказа если ссылок не осталось
            auto entityToUpdate = failureTypesCache.getEntityBySemantic(semantic);
            if (entityToUpdate.isEmpty())
                continue;

            auto &refs = entityToUpdate.refs;
            auto it = std::find(refs.begin(), refs.end(), elementSemantic);
            if (it != refs.end()) {
                std::swap(*it, refs.back());
                refs.pop_back();
            }

            // Просто удаляем вид отказа
            if (refs.empty()) {
                failureTypesCache.removeEntity(semantic, [&] (auto &&entity) {
                    deleteNode(initiatingService, entity.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
                });

                continue;
            }

            // Обновляем ссылки виду отказа
            failureTypesCache.updateEntity(entityToUpdate, [&] (auto &&entity) {
                WiPdmDictionaryData data;
                data.refs = entity.refs;

                WiUpdatePdmNodeQuery updateFailureTypeQuery;
                updateFailureTypeQuery.data = toJson(data);
                updateFailureTypeQuery.updateData = true;
                updateFailureTypeQuery.semantic = entity.semantic;
                updateNode(initiatingService, updateFailureTypeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            });
        }
    }

    void PdmService::updateElementSemanticInFailureTypeCache(
            std::size_t initiatingService,
            const std::string &oldSemantic,
            const std::string &newSemantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        auto &failureTypesCache = PlainCache.getEntitiesCache<WiPdmFailureType>();

        for (auto &failureType : failureTypesCache.getEntitiesByProject(projectSemanticByNodeSemantic(oldSemantic))) {
            auto &refs = failureType.refs;
            std::replace_if(refs.begin(), refs.end(),
                [&oldSemantic] (const std::string &semantic) {return semantic == oldSemantic;}, newSemantic);

            failureTypesCache.updateEntity(failureType, [&] (auto &&entity) {
                WiUpdatePdmNodeQuery updateFailureTypeQuery;
                WiPdmDictionaryData data;
                data.refs = entity.refs;

                updateFailureTypeQuery.semantic = entity.semantic;
                updateFailureTypeQuery.data = toJson(data);
                updateFailureTypeQuery.updateData = true;

                updateNode(initiatingService, updateFailureTypeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            });
        }
    }

    void PdmService::insertRbdInParallel(
            std::size_t initiatingService,
            const WiRbdInsertInParallel &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        validateInsertRbdInParallelQuery(query, ec);
        if (ec) return;

        auto rbd = nodeNearestAncestor(initiatingService,
                                       query.target.source,
                                       PdmRoles::RbdSchema,
                                       1,
                                       sessionPtr,
                                       ec,
                                       yield,
                                       mctx);
        if (ec) return;

        // left = chain.start.input, right = chain.end.output
        // получаем ноды к которым у нас цепочка была подключена перед тем как ее detach(вырвать)
        std::optional<std::string> left,right;
        {
            auto target_start = fetchRawNodeEntity(initiatingService,query.target.source,sessionPtr, ec, yield, mctx);
            auto target_end = fetchRawNodeEntity(initiatingService,query.target.target,sessionPtr, ec, yield, mctx);

            getRbdElementInput(initiatingService, target_start,left,sessionPtr,ec,yield,mctx);
            if(ec) return;
            getRbdElementOutput(initiatingService, target_end,right,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }

        // detach chain(target, passthrough = false) вырываем цепь оставляя разрыв
        // валидация цепи внутри !!
        detachRbdChain(initiatingService,query.target,false,sessionPtr,ec,yield,mctx);
        if(ec) return;

        // создаем группу !!! в группе по умолчанию есть разрыв
        WiRbdChain group;
        if(query.position){
            //Вставка в начало(сверху)
            if (query.chain.has_value()) {
                validateRbdChain(initiatingService, query.chain.value(), true, sessionPtr, ec, yield, mctx);
                if (ec) return;
                {
                    auto group_res = createRbdGroup(initiatingService, rbd.semantic, sessionPtr, ec, yield, mctx);
                    if(!group_res.has_value() || ec) return;
                    group = std::move(group_res.value());
                } 
                insertRbdChainIntoGroup(initiatingService, group, query.chain.value(), sessionPtr,ec,yield,mctx);
                if (ec) return;
            }
            else if (query.blocks.has_value()) {
                WiNewRbdBlocksPart blocksQuery;
                auto res = addRbdBlockGroup(initiatingService, rbd.semantic, query.blocks.value(), sessionPtr, ec, yield, mctx);
                if (!res.has_value() || ec) return;
                group = res.value();
            }
            else if (query.subs.has_value()) {
                auto res = addSubRbdGroup(initiatingService, rbd.semantic, query.subs.value(), sessionPtr, ec, yield, mctx);
                if (!res.has_value() || ec) return;
                group = res.value();
            }
            else {
                ec = make_error_code(error::invalid_input_data);
                return;
            }

            // вставляем последовательности в группу
            insertRbdChainIntoGroup(initiatingService, group, query.target, sessionPtr,ec,yield,mctx);
            if (ec) return;
        }else{
            //Вставка в конец(снизу)
            {
                auto group_res = createRbdGroup(initiatingService, rbd.semantic, sessionPtr, ec, yield, mctx);
                if(!group_res.has_value() || ec) return;
                group = std::move(group_res.value());
            }

            insertRbdChainIntoGroup(initiatingService, group, query.target, sessionPtr,ec,yield,mctx);
            if (ec) return;

            if (query.chain.has_value()) {
                validateRbdChain(initiatingService, query.chain.value(), true, sessionPtr, ec, yield, mctx);
                if (ec) return;
                insertRbdChainIntoGroup(initiatingService, group, query.chain.value(), sessionPtr,ec,yield,mctx);
                if (ec) return;
            }
            else if (query.blocks.has_value()) {
                addRbdBlocksToGroup(initiatingService,group,query.blocks.value(),sessionPtr,ec,yield,mctx);
                if (ec) return;
            }
            else if (query.subs.has_value()) {
                addSubsRbdToGroup(initiatingService,group,query.subs.value(),sessionPtr,ec,yield,mctx);
                if (ec) return;
            }
            else {
                ec = make_error_code(error::invalid_input_data);
                return;
            }
        }

        // вставляем группу со вставленными в нее target и сформированной chain на место откуда мы выдернули target
        // if left : link (left group.start)
        if(left.has_value()) {
            WiRbdLink leftToGroup;
            leftToGroup.source = left.value();
            leftToGroup.target = group.source;
            addRbdLink(initiatingService, leftToGroup, sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
        // if right : link (group.end right)
        if(right.has_value()) {
            WiRbdLink groupToRight;
            groupToRight.source = group.target;
            groupToRight.target = right.value();
            addRbdLink(initiatingService, groupToRight, sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
    }

    std::optional<WiRbdChain> PdmService::createRbdGroup(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService,semantic,PdmRoles::RbdSchema,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        WiRbdChain group;

        //create start node
        {
            WiNewPdmNodeQuery startNodeQuery;
            WiPdmRbdGroupStartExtension extension;
            startNodeQuery.parent = semantic;
            startNodeQuery.role = PdmRoles::RbdGroupStart;
            startNodeQuery.extension = toJson(extension);
            group.source = addNewNodeInternal(initiatingService, startNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return std::nullopt;
        }

        //create end node
        {
            WiNewPdmNodeQuery endNodeQuery;
            WiPdmRbdGroupEndExtension extension;
            endNodeQuery.parent = semantic;
            endNodeQuery.role = PdmRoles::RbdGroupEnd;
            endNodeQuery.extension = toJson(extension);
            group.target = addNewNodeInternal(initiatingService, endNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if (ec) return std::nullopt;
        }

        //update both nodes to link them together
        {
            WiPdmRbdGroupStartExtension extension;
            extension.end = group.target;

            WiUpdatePdmNodeQuery startNodeQuery;
            startNodeQuery.semantic = group.source;
            startNodeQuery.extension = toJson(extension);
            startNodeQuery.updateExtension = true;
            updateNode(initiatingService,startNodeQuery,sessionPtr,Filter::filterOn,ec,yield,mctx);
            if (ec) return std::nullopt;
        }

        {
            WiPdmRbdGroupEndExtension extension;
            extension.start = group.source;

            WiUpdatePdmNodeQuery endNodeQuery;
            endNodeQuery.semantic = group.target;
            endNodeQuery.extension = toJson(extension);
            endNodeQuery.updateExtension = true;
            updateNode(initiatingService,endNodeQuery,sessionPtr,Filter::filterOn,ec,yield,mctx);
            if (ec) return std::nullopt;
        }

        return group;
    }

    void PdmService::detachRbdChain(
            std::size_t initiatingService,
            const WiRbdChain &query,
            bool passthrough,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        // todo: make Internal method where validateChain Is a parameter, but from outside is always true
        bool validateChain = false;

        if(mctx.isOwner() && !passthrough){
            ec = make_error_code(error::invalid_input_data);
            return;
        }
        if(validateChain){
            validateRbdChain(initiatingService, query, false, sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
        // получаем ноды к которым у нас цепочка была подключена перед тем как ее detach(вырвать)
        std::optional<std::string> left,right;
        {
            auto target_start = fetchRawNodeEntity(initiatingService,query.source,sessionPtr, ec, yield, mctx);
            auto target_end = fetchRawNodeEntity(initiatingService,query.target,sessionPtr, ec, yield, mctx);

            if(target_start->role == PdmRoles::RbdGroupEnd) {
                std::vector<std::string> leftSet;
                getRbdElementInputs(initiatingService, target_start, leftSet, sessionPtr, ec, yield, mctx);
                if(ec) return;
                if(leftSet.size() >= 2){
                    ec = make_error_code(error::rbd_element_has_multiple_inputs);
                    return;
                }else if(leftSet.size() == 1){
                    left = *leftSet.begin();
                }else{
                    left = std::nullopt;
                }
            }else{
                getRbdElementInput(initiatingService, target_start, left, sessionPtr, ec, yield, mctx);
                if (ec) return;
            }

            if(target_end->role == PdmRoles::RbdGroupStart) {
                std::vector<std::string> rightSet;
                getRbdElementOutputs(initiatingService, target_end, rightSet, sessionPtr, ec, yield, mctx);
                if(ec) return;
                if(rightSet.size() >= 2){
                    ec = make_error_code(error::rbd_element_has_multiple_outputs);
                    return;
                }else if(rightSet.size() == 1){
                    right = *rightSet.begin();
                }   else{
                    right = std::nullopt;
                }
            }else{
                getRbdElementOutput(initiatingService, target_end, right, sessionPtr, ec, yield, mctx);
                if (ec) return;
            }

        }
        // remove links on the end of the chain
        if(left.has_value()){
            WiRbdLink leftToChain;
            leftToChain.source = left.value();
            leftToChain.target = query.source;
            removeRbdLink(initiatingService,leftToChain,sessionPtr,ec,yield,mctx);
            if (ec) return;
        } else return;
        if(right.has_value()){
            WiRbdLink chainToRight;
            chainToRight.source = query.target;
            chainToRight.target = right.value();
            removeRbdLink(initiatingService,chainToRight,sessionPtr,ec,yield,mctx);
            if (ec) return;
        } else return;

        std::shared_ptr<WiPdmRawNodeEntity> leftNode;
        leftNode  = fetchRawNodeEntity(initiatingService,left.value(),sessionPtr, ec, yield, mctx);
        if (ec) return;

        std::shared_ptr<WiPdmRawNodeEntity> rightNode;
        rightNode = fetchRawNodeEntity(initiatingService,right.value(),sessionPtr, ec, yield, mctx);
        if (ec) return;

        // add chain from left to right if needed
        if(passthrough && !(leftNode->role == PdmRoles::RbdGroupStart && rightNode->role == PdmRoles::RbdGroupEnd)){
            WiRbdLink leftToRight;
            leftToRight.source = left.value();
            leftToRight.target = right.value();
            addRbdLink(initiatingService, leftToRight,sessionPtr,ec, yield, mctx);
            if (ec) return;
        }



        if(passthrough && leftNode->role == PdmRoles::RbdGroupStart && rightNode->role == PdmRoles::RbdGroupEnd)
        {
            std::vector<std::string> leftOutputs;
            getRbdElementOutputs(initiatingService, leftNode, leftOutputs, sessionPtr, ec, yield, mctx);
            if (ec) return;

            if (leftOutputs.size() < 2)
            {
                WiRbdChain chainQuery;
                chainQuery.source = leftNode->semantic;
                chainQuery.target = leftNode->semantic;

                detachRbdChain(initiatingService, chainQuery, true, sessionPtr, ec, yield, mctx);
                if(ec) return;

                deleteNode(initiatingService, leftNode->semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
            }

            std::vector<std::string> rightInputs;
            getRbdElementInputs(initiatingService, rightNode, rightInputs, sessionPtr, ec, yield, mctx);
            if (ec) return;

            if (rightInputs.size() < 2)
            {
                WiRbdChain chainQuery;
                chainQuery.source = rightNode->semantic;
                chainQuery.target = rightNode->semantic;

                detachRbdChain(initiatingService, chainQuery, true, sessionPtr, ec, yield, mctx);
                if(ec) return;

                deleteNode(initiatingService, rightNode->semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
            }
        }
    }

    std::optional<WiRbdChain> PdmService::getRbdGroup(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        auto node_left = fetchRawNodeEntity(initiatingService,semantic,sessionPtr,ec,yield,mctx);
        if(!node_left || ec) return std::nullopt;
        if(!node_left->extension.has_value()){
            ec = make_error_code(error::invalid_rbd_element);
            return std::nullopt;
        }
        std::string link;
        switch(node_left->role){
            case PdmRoles::RbdGroupStart:{
                WiPdmRbdGroupStartExtension extension = fromJson<decltype(extension)>(node_left->extension.value());
                if(!extension.end.has_value()){
                    ec = make_error_code(error::invalid_rbd_element);
                    return std::nullopt;
                }
                link = extension.end.value();
            }
                break;
            case PdmRoles::RbdGroupEnd:{
                WiPdmRbdGroupEndExtension extension = fromJson<decltype(extension)>(node_left->extension.value());
                if(!extension.start.has_value()){
                    ec = make_error_code(error::invalid_rbd_element);
                    return std::nullopt;
                }
                link = extension.start.value();
            }
                break;
            default:
                ec = make_error_code(error::not_an_rbd_element);
                return std::nullopt;
                break;
        }

        auto node_right = fetchRawNodeEntity(initiatingService,link,sessionPtr,ec,yield,mctx);
        if(!node_right || ec) return std::nullopt;
        if(node_left->role == PdmRoles::RbdGroupEnd){
            std::swap(node_left,node_right);
        }

        if(node_left->role != PdmRoles::RbdGroupStart || node_right->role != PdmRoles::RbdGroupEnd){
            ec = make_error_code(error::invalid_rbd_group);
            return std::nullopt;
        }

        WiRbdChain group;
        group.source = node_left->semantic;
        group.target = node_right->semantic;
        return group;
    }

    void PdmService::validateInsertRbdIntoGroupQuery(
            const WiRbdInsertIntoGroup &query,
            boost::system::error_code &ec) const noexcept(true){
        if( (!query.chain.has_value()&&!query.blocks.has_value()&&!query.subs.has_value())
            || (query.chain.has_value()&&query.blocks.has_value())
            || (query.chain.has_value()&&query.subs.has_value())
            || (query.blocks.has_value()&&query.subs.has_value())){
            ec= make_error_code(error::invalid_input_data);
        }
    }

    void PdmService::insertRbdChainIntoGroup(
            std::size_t initiatingService,
            const WiRbdChain &group,
            const WiRbdChain &chain,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        // link (group.start  target.start )
        WiRbdLink groupToChain;
        groupToChain.source = group.source;
        groupToChain.target = chain.source;
        addRbdLink(initiatingService, groupToChain, sessionPtr, ec, yield, mctx);
        if (ec) return;

        // link (target.end group.end )
        WiRbdLink chainToGroup;
        chainToGroup.source = chain.target;
        chainToGroup.target = group.target;
        addRbdLink(initiatingService, chainToGroup, sessionPtr, ec, yield, mctx);
        if(ec) return;

        return;
    }

    void PdmService::insertRbdIntoGroup(
            std::size_t initiatingService,
            const WiRbdInsertIntoGroup &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        validateInsertRbdIntoGroupQuery(query, ec);
        if (ec) return;

        WiRbdChain group;
        {
            auto group_res = getRbdGroup(initiatingService, query.group, sessionPtr, ec, yield, mctx);
            if (!group_res.has_value() || ec) return;
            group = std::move(group_res.value());
        }

        if (query.chain.has_value()) {
            insertRbdChainIntoGroup(initiatingService, group, query.chain.value(), sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
        else if (query.blocks.has_value()) {
            addRbdBlocksToGroup(initiatingService, group, query.blocks.value(), sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
        else if (query.subs.has_value()) {
            addSubsRbdToGroup(initiatingService, group, query.subs.value(), sessionPtr, ec, yield, mctx);
            if (ec) return;
        }
        else {
            ec = make_error_code(error::invalid_input_data);
            return;
        }
        return;
    }

    void PdmService::BindPdmComponentWithRbdBlock(
            std::size_t initiatingService,
            const WiSemanticsPairQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.first_semantic.empty() || query.second_semantic.empty()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        auto pdmComponentSemantic = query.first_semantic;
        auto RBDBlockSemantic     = query.second_semantic;

        BindPdmComponentWithRbdBlockInternal(
                    initiatingService,
                    pdmComponentSemantic,
                    RBDBlockSemantic,
                    sessionPtr,
                    ec,
                    yield);
    }

    void PdmService::bindRbdSchemaWithSubRbd(
            std::size_t initiatingService,
            const WiSemanticsPairQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.first_semantic.empty() || query.second_semantic.empty()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        auto RbdSchemaSemantic = query.first_semantic;
        auto SubRbdSemantic     = query.second_semantic;

        BindRbdSchemaWithSubRbdInternal(
                    initiatingService,
                    RbdSchemaSemantic,
                    SubRbdSemantic,
                    sessionPtr,
                    ec,
                    yield);
    }

    void PdmService::deleteRBDs(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.semantics.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        for(const auto &rbd_semantic: query.semantics)
        {
            deleteRBD(initiatingService, WiSemanticOnlyQuery{rbd_semantic},sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }

    void PdmService::deleteRBD(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        checkNode(initiatingService, query.semantic, PdmRoles::RbdSchema, sessionPtr,  ec, yield, mctx);
        if(ec) return;

        WiPdmRawTreeNode::Container nodes;
        fetchFlatRawTree(initiatingService,query.semantic, nodes, sessionPtr, ec, yield, mctx);
        if(ec) return;

        for(const auto &node: nodes){
            if(node.role == PdmRoles::RbdBlock){
                if(node.extension.has_value()){
                    WiPdmRbdBlockExtension extension = fromJson<decltype(extension)>(node.extension.value());
                    if(extension.ref.has_value()){
                        unbindPdmComponentWithRbdBlockInternal(initiatingService,extension.ref.value(),node.semantic, sessionPtr,ec,yield,mctx);
                        if(ec) return;
                    }
                }
            }
            if(node.role == PdmRoles::SubRbd){
                if(node.extension.has_value()){
                    WiPdmSubRbdExtension extension = fromJson<decltype(extension)>(node.extension.value());
                    if(extension.ref.has_value()){
                        unbindRbdSchemaWithSubRbdInternal(initiatingService,extension.ref.value(),node.semantic, sessionPtr,ec,yield,mctx);
                        if(ec) return;
                    }
                }
            }
        }

        auto schemaNode = fetchRawNode(initiatingService, query.semantic, sessionPtr, ec, yield, mctx);
        if(ec || !schemaNode) return;

        WiPdmRbdExtension schemaExt = fromJson<WiPdmRbdExtension>(schemaNode->extension.value());
        for(const auto &ref: schemaExt.sub_rbd_ref)
        {
            unbindRbdSchemaWithSubRbdInternal(initiatingService,query.semantic,ref, sessionPtr,ec,yield,mctx);
            if(ec) return;
        }

        deleteNode(initiatingService, query.semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    void PdmService::BindPdmComponentWithRbdBlockInternal(
            std::size_t initiatingService,
            const std::string& pdmComponentSemantic,
            const std::string& RBDBlockSemantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto componentNode = fetchRawNode(initiatingService,pdmComponentSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !componentNode) return;

        if(componentNode->role)
        {
            switch(componentNode->role){
                case PdmRoles::ProxyComponent:
                case PdmRoles::Container:
                case PdmRoles::ElectricComponent:
                    break;
                default:
                    ec = make_error_code(error::invalid_node_role);
                    return;
            }
        }
        else
        {
            ec = make_error_code(error::invalid_node_role);
            return;
        }

        auto RBDBlockNode  = fetchRawNode(initiatingService,RBDBlockSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !RBDBlockNode) return;

        if(RBDBlockNode->role != PdmRoles::RbdBlock) {
            ec = make_error_code(invalid_node_role);
            return;
        }

        //Проверяем находятся ли элемент ЛСИ и блок ССН в одном проекте

        auto project_pdm = nodeNearestAncestor(initiatingService, componentNode->semantic, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec) return;

        auto project_RBD = nodeNearestAncestor(initiatingService, RBDBlockNode->semantic, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec) return;

        if(project_pdm.semantic != project_RBD.semantic){
            ec = make_error_code(error::entities_from_different_projects);
            return;
        }

        auto schema = nodeNearestAncestor(initiatingService,RBDBlockNode->semantic, PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
        if(ec)return;

        WiUpdatePdmNodeQuery updateComponentQuery(*componentNode);
        WiUpdatePdmNodeQuery updateRBDQuery(*RBDBlockNode);

        //устанавливаем ссылку с компонента на ССН
        WiPdmElementExtension componentExt;
        if(componentNode->extension.has_value()){
            componentExt = fromJson<WiPdmElementExtension>(componentNode->extension.value());

//            //если компонент ЛСИ уже имеет связь, возвращаем ошибку
//            if(componentExt.rbd_block_ref.has_value())
//            {
//                { // check all descendants
//                    WiPdmRawTreeNode::Container descendants;
//                    fetchFlatRawTree(initiatingService, componentNode->semantic, descendants, sessionPtr, ec, yield, mctx);
//                    if (ec) return;
//                    WiPdmElementExtension extension;
//                    for (const auto &descendant : descendants) {
//                        if (descendant.extension.has_value()) {
//                            extension = fromJson<decltype(extension)>(descendant.extension.value());
//                            if (extension.rbd_block_ref.has_value()) {
//                                ec = make_error_code(element_descendant_already_binded_with_rbd_block);
//                                return;
//                            }
//                        }
//                    }
//                }
//                { // check all ancestors
//                    auto curr = componentNode;
//                    while(true){
//                        boost::system::error_code tec;
//                        auto sem = parentSemantic(curr->semantic,tec);
//                        if(tec) break;
//                        curr = fetchRawNode(sem,ec,yield,mctx);
//                        if(!curr) break;
//                        if(curr->role == PdmRoles::Container
//                           || curr->role == PdmRoles::ProxyComponent
//                           || curr->role == PdmRoles::ElectricComponent){
//                            if (curr->extension.has_value()) {
//                                WiPdmElementExtension extension;
//                                extension = fromJson<decltype(extension)>(curr->extension.value());
//                                if (extension.rbd_block_ref.has_value()) {
//                                    ec = make_error_code(element_ancestor_already_binded_with_rbd_block);
//                                    return;
//                                }
//                            }
//                        }else break;
//                    }
//                }
//            }
        }
        if(!componentExt.rbd_block_refs.count(schema.semantic)){
            componentExt.rbd_block_refs.insert(std::make_pair(schema.semantic, std::set<std::string>()));
        }
        componentExt.rbd_block_refs.at(schema.semantic).insert(RBDBlockNode->semantic);

        updateComponentQuery.updateData      = false;
        updateComponentQuery.semantic        = pdmComponentSemantic;
        updateComponentQuery.extension       = toJson(componentExt);
        updateComponentQuery.updateExtension = true;

        //устанавливаем ссылку с ССН на компонент
        WiPdmRbdBlockExtension RBDBlockExt;
        if(RBDBlockNode->extension.has_value()){
            RBDBlockExt = fromJson<WiPdmRbdBlockExtension>(RBDBlockNode->extension.value());

            //если ССН уже имеет связь, удаляем её
            if(RBDBlockExt.ref.has_value())
                unbindPdmComponentWithRbdBlockInternal(initiatingService, RBDBlockExt.ref.value(), RBDBlockSemantic, sessionPtr, ec, yield, mctx);
        }

        // Заполняем переменные надежности для блока
        fillRbdBlockVars(initiatingService, pdmComponentSemantic, RBDBlockSemantic, sessionPtr, ec, yield, mctx);

        RBDBlockExt.ref = pdmComponentSemantic;

        updateRBDQuery.updateData      = false;
        updateRBDQuery.semantic        = RBDBlockSemantic;
        updateRBDQuery.extension       = toJson(RBDBlockExt);
        updateRBDQuery.updateExtension = true;

        updateNode(initiatingService, updateComponentQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;
        updateNode(initiatingService, updateRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;
        mctx.addSchemaFlagsTrigger(schema.semantic);
        initiateRecalculation(initiatingService,sessionPtr,RBDBlockSemantic,ec,yield,mctx);
    }

    void PdmService::fillRbdBlockVars(
            std::size_t initiatingService,
            const std::string &pdmComponentSemantic,
            const std::string &RBDBlockSemantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        auto RBDBlockNode  = fetchRawNodeEntity(initiatingService,RBDBlockSemantic,sessionPtr, ec, yield, mctx);
        if (ec || !RBDBlockNode) return;

        // Если семантика компонента пустая, то считаем, что нужно очистить переменные блока
        if (pdmComponentSemantic.empty()) {
            WiUpdatePdmNodeQuery updateRBDQuery(*RBDBlockNode);
            WiPdmRbdBlockData block_data;

            updateRBDQuery.data            = toJson(block_data);
            updateRBDQuery.updateData      = true;
            updateRBDQuery.semantic        = RBDBlockSemantic;

            updateNode(initiatingService, updateRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            return;
        }

        auto component = fetchRawNodeEntity(initiatingService, pdmComponentSemantic, sessionPtr, ec, yield, mctx);
        if (ec || !component) return;

        auto product = nodeNearestAncestor(initiatingService, component->semantic, PdmRoles::Product, 1, sessionPtr, ec, yield, mctx);
        if (ec) return;

        auto productNodeEntity = fetchRawNodeEntity(initiatingService, product.semantic, sessionPtr, ec, yield, mctx);
        if (ec || !productNodeEntity) return;

        if (!productNodeEntity->entity.has_value()) {
            ec = make_error_code(error::node_entity_not_found);
            return;
        }

        auto productEntity = productNodeEntity->entity.value();
        if (!productEntity.data.has_value()) {
            ec = make_error_code(error::component_invalid);
            return;
        }

        long int productLifeTime{};
        WiPdmElementData data = fromJson<WiPdmElementData>(productEntity.data.value());
        if (data.ster.has_value()) {
            auto it = data.ster->data.find("expected_life_time");
            if (it != data.ster->data.end()) {
                auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                if (gen_val != nullptr) {
                    auto val = std::get_if<std::optional<long double>>(gen_val);
                    if (val != nullptr && val->has_value()) {
                        productLifeTime = val->value();
                    }
                }
            }
        }

        auto scheme = nodeNearestAncestor(initiatingService, RBDBlockNode->semantic, PdmRoles::RbdSchema,1, sessionPtr,ec, yield,mctx);
        if (ec) return;

        if (!scheme.extension.has_value()) return;
        WiPdmRbdExtension rbd_schemaExt = fromJson<decltype(rbd_schemaExt)>(scheme.extension.value());
        if (!rbd_schemaExt.expected_life_time.has_value()) {
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        long int schemeLifeTime = rbd_schemaExt.expected_life_time.value();

        WiPdmElementVariables componentVariables;
        if (component->entity.has_value()) {
            const auto componentEntity = component->entity.value();
            if (componentEntity.data.has_value()) {
                const auto data = fromJson<WiPdmComponentData>(componentEntity.data.value());
                if (data.variables.has_value()) {
                    componentVariables = data.variables.value();
                }
            }
        }

        WiPdmElementVariables vars;
        // Если время работы изделия совпадает с временем работы схемы, то просто добавляем блоку переменные компонента
        if (productLifeTime == schemeLifeTime) {
            vars = componentVariables;
        }
        else { // Иначе необходимо пересчитать переменные с учетом времени работы схемы
            vars.failure_rate = componentVariables.failure_rate;
            fillAllVars(vars, schemeLifeTime,ec);
        }

        WiUpdatePdmNodeQuery updateRBDQuery(*RBDBlockNode);
        WiPdmRbdBlockData block_data;
        block_data.variables = vars;

        updateRBDQuery.data            = toJson(block_data);
        updateRBDQuery.updateData      = true;
        updateRBDQuery.semantic        = RBDBlockSemantic;

        updateNode(initiatingService, updateRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        initiateRecalculation(initiatingService, sessionPtr, RBDBlockSemantic,ec, yield,mctx);
    }

    void PdmService::BindRbdSchemaWithSubRbdInternal(
                std::size_t initiatingService,
                const std::string& RbdSchemaSemantic,
                const std::string& SubRbdSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto schemaNode = fetchRawNode(initiatingService,RbdSchemaSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !schemaNode) return;

        if(schemaNode->role != PdmRoles::RbdSchema) {
            ec = make_error_code(invalid_node_role);
            return;
        }

        std::string subRbdParentSemantic = parentSemantic(SubRbdSemantic, ec);
        if(ec) return;

        if (subRbdParentSemantic == RbdSchemaSemantic)
        {
            ec = make_error_code(error::sub_rbd_cant_be_binded_with_own_parent);
            return;
        }

        auto subRbdNode  = fetchRawNode(initiatingService,SubRbdSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !subRbdNode) return;

        if(subRbdNode->role != PdmRoles::SubRbd) {
            ec = make_error_code(invalid_node_role);
            return;
        }

        WiUpdatePdmNodeQuery updateShemaQuery(*schemaNode);
        WiUpdatePdmNodeQuery updateSubRBDQuery(*subRbdNode);

        //устанавливаем ссылку с CCН-схемы на ССН-свёртку
        WiPdmRbdExtension schemaExt = fromJson<WiPdmRbdExtension>(schemaNode->extension.value());
        schemaExt.sub_rbd_ref.insert(SubRbdSemantic);


        updateShemaQuery.updateData      = false;
        updateShemaQuery.semantic        = RbdSchemaSemantic;
        updateShemaQuery.extension       = toJson(schemaExt);
        updateShemaQuery.updateExtension = true;


        //устанавливаем ссылку с ССН на компонент
        WiPdmSubRbdExtension subRbdExt;
        if(subRbdNode->extension.has_value()){
            subRbdExt = fromJson<WiPdmSubRbdExtension>(subRbdNode->extension.value());

            //если ССН уже имеет связь, удаляем её
            if(subRbdExt.ref.has_value())
            {
                unbindRbdSchemaWithSubRbdInternal(
                            initiatingService,
                            subRbdExt.ref.value(),
                            SubRbdSemantic,
                            sessionPtr,
                            ec,
                            yield,
                            mctx);
                if(ec) return;
            }
        }

        subRbdExt.ref = RbdSchemaSemantic;

        updateSubRBDQuery.updateData      = false;
        updateSubRBDQuery.semantic        = SubRbdSemantic;
        updateSubRBDQuery.extension       = toJson(subRbdExt);
        updateSubRBDQuery.updateExtension = true;

        updateNode(initiatingService, updateShemaQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;
        updateNode(initiatingService, updateSubRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;
        mctx.addSchemaFlagsTrigger(subRbdParentSemantic);
        initiateRecalculation(initiatingService,sessionPtr,SubRbdSemantic,ec,yield,mctx);
    }

    void PdmService::unbindAllDescendantsFromRbd(
            std::size_t initiatingService,
            const std::string &element,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        WiPdmRawTreeNode::Container container;
        fetchFlatRawTree(initiatingService,element, container, sessionPtr, ec, yield, mctx);
        if(ec) return;
        for(const auto &node: container){
            removeRbdBindingFromElement(initiatingService, node.semantic, sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }
    std::optional<std::vector<std::string>> PdmService::removeRbdBindingFromElement(
            std::size_t initiatingService,
            const std::string &element,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto node = fetchRawNode(initiatingService,element,sessionPtr, ec, yield, mctx);
        if(ec || !node) return std::nullopt;

        std::vector<std::string> ret;
        if(node->role == PdmRoles::ElectricComponent || node->role == PdmRoles::ProxyComponent
            || node->role == PdmRoles::Container){
            if(!node->extension.has_value()) {
                ec = make_error_code(error::element_invalid);
                return std::nullopt;;
            }
            WiPdmElementExtension extension = fromJson<decltype(extension)>(node->extension.value());
            for(const auto& schema:extension.rbd_block_refs){
                for(const auto& block:schema.second) {
                    unbindPdmComponentWithRbdBlockInternal(initiatingService,
                                                           element,
                                                           block,
                                                           sessionPtr,
                                                           ec,
                                                           yield,
                                                           mctx);
                    if(ec) return std::nullopt;
                    ret.push_back(block);
                }
            }
            if(ret.empty())
                return std::nullopt;
            else
                return ret;
        }
        else
        {
            return std::nullopt;
        }
    }

    void PdmService::unbindPdmComponentWithRBDBlock(
            std::size_t initiatingService,
            const WiSemanticsPairQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.first_semantic.empty() || query.second_semantic.empty()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        auto& pdmComponentSemantic = query.first_semantic;
        auto& RBDBlockSemantic     = query.second_semantic;

        unbindPdmComponentWithRbdBlockInternal(
                    initiatingService,
                    pdmComponentSemantic,
                    RBDBlockSemantic,
                    sessionPtr,
                    ec,
                    yield);
    }

    void PdmService::unbindPdmComponentWithRbdBlockInternal(
            std::size_t initiatingService,
            const std::string& pdmComponentSemantic,
            const std::string& RBDBlockSemantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto componentNode = fetchRawNode(initiatingService,pdmComponentSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !componentNode) return;

        switch(componentNode->role){
            case PdmRoles::ProxyComponent:
            case PdmRoles::Container:
            case PdmRoles::ElectricComponent:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return;
        }

        auto RBDBlockNode  = fetchRawNode(initiatingService,RBDBlockSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !RBDBlockNode) return;

        if(RBDBlockNode->role != PdmRoles::RbdBlock) {
            ec = make_error_code(invalid_node_role);
            return;
        }


        //Проверяем связаны ли эти элемент ЛСИ и блок ССН друг с другом
        WiPdmElementExtension componentExt;
        if(componentNode->extension.has_value()){
            componentExt = fromJson<WiPdmElementExtension>(componentNode->extension.value());
        }
        else
        {
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        WiPdmRbdBlockExtension RBDBlockExt;
        if(RBDBlockNode->extension.has_value()){
            RBDBlockExt = fromJson<WiPdmRbdBlockExtension>(RBDBlockNode->extension.value());
        }
        else
        {
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        if((componentExt.rbd_block_refs.empty()) || !(RBDBlockExt.ref.has_value()))
        {
//            ec = make_error_code(error::no_work_to_do);
            return;
        }

//        if(componentExt.rbd_block_ref.value() != RBDBlockSemantic ||
//           RBDBlockExt.ref.value() != pdmComponentSemantic)
//        {
//            ec = make_error_code(error::no_work_to_do);
//            return;
//        }


        WiUpdatePdmNodeQuery updateComponentQuery(*componentNode);
        WiUpdatePdmNodeQuery updateRBDQuery(*RBDBlockNode);

        //удаляем ссылку с ССН на компонент
        RBDBlockExt.ref = std::nullopt;

        auto schema = nodeNearestAncestor(initiatingService,RBDBlockSemantic,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
        if(ec) return;

        if(!componentExt.rbd_block_refs.count(schema.semantic)){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        auto &refs = componentExt.rbd_block_refs.at(schema.semantic);
        if(!refs.count(RBDBlockSemantic)){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        refs.erase(RBDBlockSemantic);

        updateComponentQuery.updateData      = false;
        updateComponentQuery.semantic        = pdmComponentSemantic;
        updateComponentQuery.extension       = toJson(componentExt);
        updateComponentQuery.updateExtension = true;

        updateRBDQuery.updateData      = false;
        updateRBDQuery.semantic        = RBDBlockSemantic;
        updateRBDQuery.extension       = toJson(RBDBlockExt);
        updateRBDQuery.updateExtension = true;

        fillRbdBlockVars(initiatingService, {}, RBDBlockSemantic, sessionPtr, ec, yield, mctx);

        updateNode(initiatingService, updateComponentQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) return;
        updateNode(initiatingService, updateRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) return;
        mctx.addSchemaFlagsTrigger(schema.semantic);
        if (ec) return;
        initiateRecalculation(initiatingService,sessionPtr,RBDBlockSemantic,ec,yield,mctx);
    }

    void PdmService::unbindRbdSchemaWithSubRbd(
            std::size_t initiatingService,
            const WiSemanticsPairQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.first_semantic.empty() || query.second_semantic.empty()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        auto RbdSchemaSemantic = query.first_semantic;
        auto SubRbdSemantic     = query.second_semantic;

        unbindRbdSchemaWithSubRbdInternal(
                    initiatingService,
                    RbdSchemaSemantic,
                    SubRbdSemantic,
                    sessionPtr,
                    ec,
                    yield);
    }

    void PdmService::unbindRbdSchemaWithSubRbdInternal(
                std::size_t initiatingService,
                const std::string& RbdSchemaSemantic,
                const std::string& SubRbdSemantic,
                const std::shared_ptr<IWiSession> sessionPtr,
                boost::system::error_code &ec,
                const boost::asio::yield_context &yield,
                std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto schemaNode = fetchRawNode(initiatingService,RbdSchemaSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !schemaNode) return;

        if(schemaNode->role != PdmRoles::RbdSchema) {
            ec = make_error_code(invalid_node_role);
            return;
        }

        auto subRbdNode  = fetchRawNode(initiatingService,SubRbdSemantic,sessionPtr, ec, yield, mctx);
        if(ec || !subRbdNode) return;

        if(subRbdNode->role != PdmRoles::SubRbd) {
            ec = make_error_code(invalid_node_role);
            return;
        }

        WiPdmRbdExtension schemaExt = fromJson<WiPdmRbdExtension>(schemaNode->extension.value());

        //удаляем ссылку с CCН-схемы на ССН-свёртку
        if(schemaExt.sub_rbd_ref.count(SubRbdSemantic)){
            schemaExt.sub_rbd_ref.erase(SubRbdSemantic);
        }
        else
        {
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        WiPdmSubRbdExtension subRbdExt;
        if(subRbdNode->extension.has_value()){
            subRbdExt = fromJson<WiPdmSubRbdExtension>(subRbdNode->extension.value());
        }
        else
        {
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        if(subRbdExt.ref.has_value())
        {
            if(subRbdExt.ref.value() != RbdSchemaSemantic)
            {
                ec = make_error_code(error::no_work_to_do);
                return;
            }
        }
        else
        {
            ec = make_error_code(error::no_work_to_do);
            return;
        }


        WiUpdatePdmNodeQuery updateShemaQuery(*schemaNode);
        WiUpdatePdmNodeQuery updateSubRBDQuery(*subRbdNode);


        updateShemaQuery.updateData      = false;
        updateShemaQuery.semantic        = RbdSchemaSemantic;
        updateShemaQuery.extension       = toJson(schemaExt);
        updateShemaQuery.updateExtension = true;


        //удаляем ссылку с ССН-свёртки на ССН-схему
        subRbdExt.ref = std::nullopt;

        updateSubRBDQuery.updateData      = false;
        updateSubRBDQuery.semantic        = SubRbdSemantic;
        updateSubRBDQuery.extension       = toJson(subRbdExt);
        updateSubRBDQuery.updateExtension = true;

        updateNode(initiatingService, updateShemaQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) return;
        updateNode(initiatingService, updateSubRBDQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);

        auto subRbdParent = nodeNearestAncestor(initiatingService,SubRbdSemantic,PdmRoles::RbdSchema,1,sessionPtr,ec,yield,mctx);
        if(ec) return;
        mctx.addSchemaFlagsTrigger(subRbdParent.semantic);
        initiateRecalculation(initiatingService,sessionPtr,SubRbdSemantic,ec,yield,mctx);
    }

    void PdmService::ungroupSubRbd(
            std::size_t initiatingService,
            WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService,query.semantic, PdmRoles::SubRbd,sessionPtr,  ec, yield, mctx);
        if (ec) return;

        auto subRbdNode  = fetchRawNode(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec || !subRbdNode) return;

        // left_of_sub = subRbdNode.input, right_of_sub = subRbdNode.output
        // получаем ноды к которым у нас свёртка была подключена перед тем как её удалить
        std::optional<std::string> left_of_sub, right_of_sub;
        {
            auto subRbdEntity = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
            getRbdElementInput(initiatingService, subRbdEntity,left_of_sub,sessionPtr,ec,yield,mctx);
            if(ec) return;
            getRbdElementOutput(initiatingService, subRbdEntity,right_of_sub,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }


        std::optional<std::string> schemas_content_left, schemas_content_right;
        bool is_schema_not_empty = false;
        //сюда вставим копию внутрянки схемы
        std::optional<WiRbdChain> ref_schema_content_copy;

        WiPdmSubRbdExtension subRbdExt;
        if(subRbdNode->extension.has_value()){
            subRbdExt = fromJson<WiPdmSubRbdExtension>(subRbdNode->extension.value());

            //если ССН уже имеет связь, удаляем её
            if(subRbdExt.ref.has_value())
            {
                std::string schemaSemantic = subRbdExt.ref.value();

                unbindRbdSchemaWithSubRbdInternal(
                            initiatingService,
                            subRbdExt.ref.value(),
                            query.semantic,
                            sessionPtr,
                            ec,
                            yield,
                            mctx);
                if(ec) return;

                //получаем старт и конец схемы
                auto start = nodeNearestDescendant(initiatingService, schemaSemantic, PdmRoles::RbdInputNode, 1, sessionPtr, ec, yield, mctx);
                if(ec) return;

                auto end = nodeNearestDescendant(initiatingService, schemaSemantic, PdmRoles::RbdOutputNode, 1, sessionPtr, ec, yield, mctx);
                if(ec) return;


                // получаем output и input старта и конца схемы соответственно
                {
                    auto start_entity = fetchRawNodeEntity(initiatingService,start.semantic,sessionPtr, ec, yield, mctx);
                    auto end_entity = fetchRawNodeEntity(initiatingService,end.semantic,sessionPtr, ec, yield, mctx);

                    getRbdElementOutput(initiatingService,start_entity,schemas_content_left,sessionPtr,ec,yield,mctx);
                    if(ec) return;
                    getRbdElementInput(initiatingService,end_entity,schemas_content_right,sessionPtr,ec,yield,mctx);
                    if(ec) return;
                }

                //вырезаем содержимое схемы
                if(schemas_content_left.has_value() && schemas_content_right.has_value())
                {
                    //если она не пустая
                    if(schemas_content_left.value() != end.semantic)
                    {
                        is_schema_not_empty = true;

                        WiRbdChain chainQuery;
                        chainQuery.source = schemas_content_left.value();
                        chainQuery.target = schemas_content_right.value();

                        std::string parent = parentSemantic(query.semantic, ec);
                        if(ec) return;

                        ref_schema_content_copy = copyRbdChainInternal(initiatingService,
                                                                 parent,
                                                                 chainQuery,
                                                                 sessionPtr, ec, yield, mctx);
                        if(ec) return;

                        detachRbdChain(initiatingService, chainQuery, true, sessionPtr, ec, yield, mctx);
                        if(ec) return;
                    }
                }


                WiSemanticOnlyQuery _schemaSemantic{schemaSemantic};
                deleteRBD(initiatingService, _schemaSemantic,sessionPtr,ec,yield,mctx);
            }
        }

        deleteSubRbd(initiatingService,query,sessionPtr,ec, yield, mctx);

        // вставляем вырезанную цепочку из схемы (если он была не пуста) туда, откуда удалили свёртку
        if (is_schema_not_empty)
        {
            WiRbdLink link;
            link.source = left_of_sub.value();
            link.target = right_of_sub.value();

            insertRbdBetween(initiatingService,link,ref_schema_content_copy.value(),sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }

    std::optional<WiSemanticsResult> PdmService::addFunctionalUnits(
            std::size_t initiatingService,
            const WiNewFunctionalUnits &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.semantic, PdmRoles::Product, sessionPtr,  ec, yield, mctx);
        if (ec) return std::nullopt;

        //check that all of headers are not empty
        if (!std::all_of(query.elements.begin(),query.elements.end(), [](const auto& p){return checkTranslateTextContainerNotEmpty(p.name);}))
        {
            ec = make_error_code(error::no_input_data);
            return std::nullopt;
        }


        std::vector<std::string> headersStrings{};
        headersStrings.reserve(query.elements.size());
        for (const auto& element: query.elements)
        {
            headersStrings.push_back(convertTranslateTextContainerToString(element.name));
        }

        std::sort(headersStrings.begin(), headersStrings.end());
        auto pos = std::adjacent_find(headersStrings.begin(), headersStrings.end());
        if (pos != headersStrings.end())
        {
            ec = make_error_code(error::input_has_same_headers);
            return std::nullopt;
        }
        std::set<std::string> inputHeadersSet(headersStrings.begin(), headersStrings.end());


        auto functionalUnitsNode = nodeNearestDescendant(initiatingService, query.semantic,
                                                       PdmRoles::FunctionalUnits, 1, sessionPtr, ec,
                                                       yield, mctx);

        auto functionalUnitsNodeEntity = fetchRawNodeEntity(initiatingService,functionalUnitsNode.semantic,sessionPtr,ec,yield,mctx);
        WiPdmFunctionalUnitsData FUsData = fromJson<WiPdmFunctionalUnitsData>(functionalUnitsNodeEntity->entity->data.value());

        if(FUsData.headers.has_value())
        {
            std::vector<std::string> intersections;
            std::set_intersection(FUsData.headers.value().begin(), FUsData.headers.value().end(),
                                  inputHeadersSet.begin(), inputHeadersSet.end(),
                                  std::back_inserter(intersections));

            if(!intersections.empty())
            {
                ec = make_error_code(error::header_already_exists);
                return std::nullopt;
            };
        }
        else
        {
            FUsData.headers = std::make_optional<std::set<std::string>>();
        }



        if(ec) return std::nullopt;
        WiPdmFunctionalUnitsExtension functionalUnitsExtension;
        std::vector<WiPdmFunctionalUnitExtension> functionalUnitExtensions(query.elements.size());
        if(!functionalUnitsNode.extension.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        functionalUnitsExtension = fromJson<decltype(functionalUnitsExtension)>(functionalUnitsNode.extension.value());
        for (size_t i = 0; i < functionalUnitExtensions.size(); i++)
        {
            functionalUnitExtensions[i].index = functionalUnitsExtension.index++;
        }

        {
            WiUpdatePdmNodeQuery updateFUsNodeQuery(functionalUnitsNode);
            updateFUsNodeQuery.extension = toJson(functionalUnitsExtension);
            updateFUsNodeQuery.updateExtension = true;

            FUsData.headers.value().merge(inputHeadersSet);

            updateFUsNodeQuery.updateData = true;
            updateFUsNodeQuery.data = toJson(FUsData);

            updateNode(initiatingService, updateFUsNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec){
                return std::nullopt;
            }
        }

        std::vector<WiNewPdmNodeQuery> newNodeQuerys{};
        newNodeQuerys.reserve(query.elements.size());
        for (size_t i = 0; i < query.elements.size(); i++)
        {
            WiNewPdmNodeQuery newNodeQuery;
            newNodeQuery.header      = query.elements[i].name;
            newNodeQuery.description = query.elements[i].description.value_or(emptyTranslateText());

            newNodeQuery.parent = functionalUnitsNode.semantic;
            newNodeQuery.role = PdmRoles::FunctionalUnit;
            newNodeQuery.extension = toJson(functionalUnitExtensions[i]);

            WiPdmFunctionalUnitData data;
            newNodeQuery.data = toJson(data);
            newNodeQuerys.push_back(newNodeQuery);
        }

        WiSemanticsResult result;
        for (WiNewPdmNodeQuery& newNodeQuery: newNodeQuerys)
        {
            {
                auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if(ec){
                    return std::nullopt;
                }
                result.semantics.push_back(std::move(selfSemantic));
            }
        }

        return std::make_optional<WiSemanticsResult>(std::move(result));
    }

    void PdmService::deleteFunctionalUnits(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        if(query.semantics.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return;
        }

        for(const auto& semantic: query.semantics)
        {
            checkNode(initiatingService,semantic, PdmRoles::FunctionalUnit, sessionPtr,  ec, yield, mctx);
            if(ec) return;
        }

        auto functionalUnitsNode = nodeNearestAncestor(initiatingService, query.semantics.front(),
                                                           PdmRoles::FunctionalUnits, 1, sessionPtr, ec,
                                                           yield, mctx);
        if(ec) return;

        auto functionalUnitsNodeEntity   = fetchRawNodeEntity(initiatingService,functionalUnitsNode.semantic,sessionPtr,ec,yield,mctx);
        if(ec) return;
        WiPdmFunctionalUnitsData FUsData = fromJson<WiPdmFunctionalUnitsData>(functionalUnitsNodeEntity->entity->data.value());
        WiUpdatePdmNodeQuery updateFUsNodeQuery(functionalUnitsNode);

        for(const auto& semantic: query.semantics)
        {
            auto FU_refs = getFunctionalUnitRefs_internal(initiatingService,semantic,sessionPtr,ec, yield, mctx);

            //отвязываем удаляемый FU от PDM-элементов
            if (!FU_refs.empty())
            {
                std::list<WiUpdatePdmNodeQuery> elem_Updates;
                {
                    eraseFunctionalUnitFromElements_internal(
                                                        initiatingService,
                                                        semantic,
                                                        FU_refs,
                                                        elem_Updates,
                                                        sessionPtr,
                                                        ec, yield, mctx);
                    if(ec) return;
                }
                for(auto& elem_Update: elem_Updates)
                {
                    updateNode(initiatingService, elem_Update, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    if(ec) return;
                }
            }


            auto FUnode = fetchRawNode(initiatingService,semantic,sessionPtr, ec, yield, mctx);
            if(ec) return;
            std::string FUheader = convertTranslateTextContainerToString(FUnode->header);

            if (FUsData.headers.value().count(FUheader)) {
                // erase
                FUsData.headers.value().erase(FUheader);

                updateFUsNodeQuery.updateData = true;
                updateFUsNodeQuery.data = toJson(FUsData);
            }

            deleteNode(initiatingService, semantic, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return;
        }

        updateNode(initiatingService, updateFUsNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    void PdmService::appendFunctionalUnitsToElements(
            std::size_t initiatingService,
            const WiAppendFunctionalUnitsToElements &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        if(!query.semantics.size()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        for(auto semantic : query.semantics ){
            appendFunctionalUnitsToElement(initiatingService,query.functional_units,semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }

    void PdmService::appendFunctionalUnitsToElement(
            std::size_t initiatingService,
            const std::set<std::string>& functionalUnits,
            const std::string& semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
//        boost::ignore_unused(initiatingService,functional_units,semantic,sessionPtr,ec,yield);
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,semantic,sessionPtr,ec,yield,mctx);
        if(ec) return;
        switch(node->role){
            case PdmRoles::ElectricComponent:
            case PdmRoles::Container:
            case PdmRoles::ProxyComponent:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return;
        }
        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }


        WiPdmElementData data = fromJson<WiPdmElementData>(node->entity->data.value());
        WiUpdatePdmNodeQuery updateQuery(*node);
        {
            updateQuery.updateData = appendFunctionalUnits(initiatingService,data,functionalUnits,sessionPtr,ec,yield,mctx);
            if(ec) return;
            if(updateQuery.updateData) {
                updateQuery.data = toJson(data);
            }
        }

        //добавляем PDM-элемент к FU (не наоборот)
        std::list<WiUpdatePdmNodeQuery> FUupdates;
        {
            appendPdmElementToFunctionalUnits_internal(
                                                initiatingService,
                                                semantic,
                                                functionalUnits,
                                                FUupdates,
                                                sessionPtr,
                                                ec, yield, mctx);
            if(ec) return;
        }
        for(auto& FUupdate: FUupdates)
        {
            updateNode(initiatingService, FUupdate, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return;
        }
        updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
    }

    std::optional<WiSemanticsResult> PdmService::fetchFunctionalUnitsOfPdmElement(
            std::size_t initiatingService,
            WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::ignore_unused(initiatingService,sessionPtr);
        GUARD_PDM_METHOD();

        auto node = fetchRawNode(query.semantic, ec, yield, mctx);
        if(ec || !node) return std::nullopt;;
        switch(node->role) {
            case PdmRoles::ElectricComponent:
            case PdmRoles::Container:
            case PdmRoles::ProxyComponent:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return std::nullopt;;
                break;
        }

        auto FUs = getFunctionalUnitsOfElement_internal(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);

        WiSemanticsResult res;
        if (!FUs.empty())
        {
            res.semantics.reserve(FUs.size());
            std::copy(FUs.begin(), FUs.end(), std::back_inserter(res.semantics));

            return res;
        }
        else
        {
            return res;
        }
    }

    std::optional<WiSemanticsResult> PdmService::fetchPdmElementsOfFunctionalUnit(
            std::size_t initiatingService,
            WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::ignore_unused(initiatingService,sessionPtr);
        GUARD_PDM_METHOD();

        checkNode(initiatingService, query.semantic, PdmRoles::FunctionalUnit, sessionPtr,  ec, yield, mctx);
        if(ec) return std::nullopt;

        auto FU_refs = getFunctionalUnitRefs_internal(initiatingService,query.semantic,sessionPtr,ec, yield, mctx);

        WiSemanticsResult res;
        if (!FU_refs.empty())
        {
            res.semantics.reserve(FU_refs.size());
            std::copy(FU_refs.begin(), FU_refs.end(), std::back_inserter(res.semantics));

            return res;
        }
        else
        {
            return res;
        }
    }

    std::optional<WiSemanticResult> PdmService::addProduct(
            std::size_t initiatingService,
            const WiNewProduct &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        WiNewPdmNodeQuery newNodeQuery;

        checkNode(initiatingService, query.parent, PdmRoles::ProjectComposition,sessionPtr,  ec, yield, mctx);
        if (ec) return std::nullopt;
        newNodeQuery.parent = query.parent;

        newNodeQuery.header = std::move(query.header);
        newNodeQuery.description = std::move(query.description);

        if (query.data.has_value()) {
            newNodeQuery.data = std::make_optional(query.data.value());
        }
        WiPdmElementExtension ext;
        WiPdmElementData data;
        data.variables = WiPdmElementVariables{};
        data.ster = std::make_optional<typename decltype(data.ster)::value_type>();
        data.ster->data = WiStereotypeDataMap{{
            std::make_pair("expected_life_time", WiStereotypeParameterDefinition(
                "system", "", true, WiValue(WiValueGeneral{
                    std::optional<long double>(100000)
                })
            )),
            std::make_pair("MDM::SETTINGS::PARAMETERS::ambient_temperature", WiStereotypeParameterDefinition(
                "system", "", true, WiValue(WiValueGeneral{
                    std::optional<long double>(std::nullopt)
                })
            ))
        }};
        // todo: add icon
        newNodeQuery.extension = toJson(ext);
        newNodeQuery.role = PdmRoles::Product;
        newNodeQuery.data = toJson(data);
        auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) {
            return std::nullopt;
        }

        WiNewPdmNodeQuery newFunctionalUnitsNodeQuery;
        WiPdmFunctionalUnitsExtension fusExtension;
        fusExtension.index = 1;
        newFunctionalUnitsNodeQuery.role = PdmRoles::FunctionalUnits;
        newFunctionalUnitsNodeQuery.parent = selfSemantic;
        newFunctionalUnitsNodeQuery.header = getDefaultPdmFunctionalUnitsHeader();
        newFunctionalUnitsNodeQuery.description = getDefaultPdmFunctionalUnitsDescription();
        newFunctionalUnitsNodeQuery.extension = toJson(fusExtension);

        WiPdmFunctionalUnitsData FUsData;
        newFunctionalUnitsNodeQuery.data = toJson(FUsData);
        auto selfFunctionalUnitsSemantic = addNewNodeInternal(initiatingService, newFunctionalUnitsNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }

        WiSemanticResult result;
        result.semantic = std::move(selfSemantic);
        return std::make_optional<WiSemanticResult>(std::move(result));
    }

    std::optional<WiSemanticsResult> PdmService::copyElementsInternal(
            std::size_t initiatingService,
            const WiAddElementsToProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        if(query.sources.size() == 0){
            ec = make_error_code(error::no_work_to_do);
            return std::nullopt;
        }
        GUARD_PDM_METHOD();

        std::string destination = query.destination;
        if(query.prepend.has_value()){
            if(query.prepend.value()){
                destination = parentSemantic(query.destination, ec);
                if(ec) return std::nullopt;
            }
        }
        // check that the node is inside of project
        nodeNearestAncestor(initiatingService, destination, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec){
            ec = make_error_code(error::component_cant_be_added_outside_of_project);
            return std::nullopt;
        }
        // check type of entity that trying to create
        enum SourceType{
            PdmContainerCopy,
            Electric,
            Proxy
        } type;
        WiSemanticsResult result;
        for(const auto &elem:query.sources) {
            if (elem.has_value()) {
                std::string semantic = elem.value();
                auto node = fetchRawNodeEntity(initiatingService,semantic,sessionPtr, ec, yield, mctx);
                if (ec) return std::nullopt;
                if (!node) {
                    ec = make_error_code(error::node_not_found);
                    return std::nullopt;
                }
                switch (node->role) {
                    case PdmRoles::ProxyComponent:
                        type = Proxy;
                        break;
                    case PdmRoles::ElectricComponent:
                        type = Electric;
                        break;
                    case PdmRoles::Container:
                        type = PdmContainerCopy;
                        break;
                    default:
                        ec = make_error_code(error::invalid_node_role);
                        return std::nullopt;
                        break;
                }
                WiAddComponentWithData addQuery;
                WiPdmElementExtension ext;
                node = fetchRawNodeEntity(initiatingService,elem.value(),sessionPtr, ec, yield, mctx);
                ext = fromJson<WiPdmElementExtension>(node->extension.value());

                ext.letter_tag.erase(std::remove_if(ext.letter_tag.begin(),ext.letter_tag.end(),isspace),ext.letter_tag.end());
                auto rit1 = std::find_if_not(ext.letter_tag.rbegin(),ext.letter_tag.rend(),isdigit);
                std::string letter_part(ext.letter_tag.begin(),rit1.base());

                addQuery.letter_tag=letter_part;

                addQuery.destination = query.destination;

                addQuery.designation = ext.designation;

                addQuery.description = node->description;

                addQuery.header = node->header;

                WiSemanticOnlyQuery sq;
                sq.semantic =  semantic;
                auto res_el = fetchComponentData(initiatingService,sq,sessionPtr,ec,yield,mctx);
                if(ec || !res_el.has_value()) return std::nullopt;
                addQuery.data.variables = res_el->variables;

                if(type == SourceType::Electric && res_el.has_value())
                {
                    addQuery.data = res_el.value();
                }

                addQuery.prepend = false;

                if (query.prepend.has_value()) {
                    addQuery.prepend = query.prepend.value();
                }

                std::optional<WiSemanticResult> res;

                switch (type) {
                    case Proxy:
                        res = addProxyComponentToProjectInternal(initiatingService,addQuery,sessionPtr,ec,yield,mctx);
                        break;
                    case Electric:
                        res = addComponentToProjectInternal(initiatingService,addQuery,sessionPtr,ec,yield,mctx);
                        break;
                    case PdmContainerCopy:
                        res = addContainerToProjectInternal(initiatingService,addQuery,sessionPtr,ec,yield,mctx);
                        if(ec) return std::nullopt;

                        //todo: copy descendants
                        if(res.has_value()){
                            copyAllDescendantsTo(initiatingService, elem.value(), res.value().semantic, sessionPtr, ec, yield, mctx);
                        }
                        break;
                    default:
                        ec = make_error_code(error::cant_create_element);
                        return std::nullopt;
                        break;
                }
                if(ec || !res.has_value()){
                    if(!ec){
                        ec = make_error_code(error::cant_create_element);
                    }
                    return std::nullopt;
                }
                result.semantics.push_back(res.value().semantic);
            }
        }
        if(ec) return std::nullopt;
        return result;
    }

    std::optional<WiSemanticResult> PdmService::deleteElementToBin(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        WiPdmBinElementExtension bin_ext;

        auto res=deleteElementToBinInternal(initiatingService,semantic,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        auto parent_semantic = parentSemantic(semantic,ec);
        if(ec) return std::nullopt;

        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = false;
        rearrangeQuery.destination = parent_semantic;
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;

        return res;
    }

    void PdmService::prepareElementForDeletionToBin (
            std::size_t initiatingService,
            const WiPdmRawNodeEntity &node,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
       if(node.role == PdmRoles::Container){
           WiPdmRawNodeEntity::Container container;
           fetchRawNodesEntity(initiatingService,node.semantic,container,sessionPtr,ec,yield,mctx);
           if(ec) return;
           for(const auto &descendant:container){
               prepareElementForDeletionToBin(initiatingService,descendant,sessionPtr,ec,yield,mctx);
               if(ec) return;
           }
       }

        if (node.role == PdmRoles::ElectricComponent ||
            node.role == PdmRoles::Container ||
            node.role == PdmRoles::ProxyComponent)
        {
            auto FUs = getFunctionalUnitsOfElement_internal(initiatingService,node.semantic,sessionPtr, ec, yield, mctx);

            if (!FUs.empty())
            {
                //удаляем PDM-элемент из FU (не наоборот)
                std::list<WiUpdatePdmNodeQuery> FUupdates;
                {
                    erasePdmElementFromFunctionalUnits_internal(
                        initiatingService,
                        node.semantic,
                        FUs,
                        FUupdates,
                        sessionPtr,
                        ec, yield, mctx);
                    if(ec) return;
                }
                for(auto& FUupdate: FUupdates)
                {
                    updateNode(initiatingService, FUupdate, sessionPtr, Filter::filterOn, ec, yield, mctx);
                    if(ec) return;
                }
            }
        }

        switch(node.role){
            case PdmRoles::ElectricComponent:
            case PdmRoles::Container:
            case PdmRoles::ProxyComponent:
                break;
            case PdmRoles::BlankComponent:
                return;
                break;
            default:
                ec = make_error_code(error::cant_delete_element_to_bin);
                return;
        }
        if(!node.extension.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }

        if(node.role == PdmRoles::ElectricComponent
        || node.role == PdmRoles::ProxyComponent
        || node.role == PdmRoles::Container){
            removeRbdBindingFromElement(initiatingService,node.semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }
    std::optional<WiSemanticResult> PdmService::deleteElementToBinInternal (
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,semantic,sessionPtr,ec,yield,mctx);
        if(ec || !node){
            return std::nullopt;
        }
        if(node->role ==PdmRoles::BlankComponent){
            deleteNode(initiatingService,semantic,sessionPtr,Filter::filterOn,ec,yield,mctx);
            if(ec) return std::nullopt;
            return WiSemanticResult{semantic};
        }

        prepareElementForDeletionToBin(initiatingService,*node,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;

        node = fetchRawNodeEntity(initiatingService,node->semantic,sessionPtr,ec,yield,mctx);
        if(ec || !node){
            return std::nullopt;
        }

        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
        }
        auto project = nodeNearestAncestor(initiatingService, node->semantic, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }
        auto bin = nodeNearestDescendant(initiatingService, project.semantic, PdmRoles::ProjectElementBin, 1, sessionPtr, ec, yield, mctx);
        if(ec){
            return std::nullopt;
        }
        if(!bin.extension.has_value()){
            ec = make_error_code(error::element_bin_invalid);
            return std::nullopt;
        }
        WiPdmBinExtension binExtension = fromJson<WiPdmBinExtension>(bin.extension.value());
        if(ec){
            return std::nullopt;
        }

        WiPdmElementExtension ext = fromJson<WiPdmElementExtension>(node->extension.value());

        WiMovePdmNodeQuery moveQuery;
        moveQuery.semantic = node->semantic;
        moveQuery.destination = bin.semantic;
        auto res = moveNodeInternal(initiatingService,moveQuery,sessionPtr, Filter::filterOn, ec,yield,mctx);
        if(ec || !res) return std::nullopt;

        // assign index
        {
            bool updateBin = false;
            std::vector<std::uint32_t> pos;
            if (!ext.bin_index.has_value()) {
                pos.push_back(++(binExtension.index));
                updateBin = true;
            }
            else {
                bool rest = unwrapPositional(pos, ext.bin_index.value());
                if ( !rest ||  pos.size() != 1) {
                    ec = make_error_code(error::cant_assign_positional_index);
                    return std::nullopt;
                }
            }
            assignPositional(initiatingService, pos, res->semantic, sessionPtr, ec, yield, 1, mctx);
            if (ec) return std::nullopt;

            //update bin
            if (updateBin) {
                WiUpdatePdmNodeQuery updateBinQuery(bin);
                updateBinQuery.extension = toJson(binExtension);
                updateBinQuery.updateExtension = true;
                updateNode(initiatingService, updateBinQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if (ec) return std::nullopt;
            }
        }

        // update
        WiPdmBinElementExtension bin_ext;
        bin_ext.prepend = false;
        bin_ext.location = parentSemantic(node->semantic,ec);
        node = fetchRawNodeEntity(initiatingService,res->semantic,sessionPtr,ec,yield,mctx);
        if(ec || !node) return std::nullopt;
        ext = fromJson<WiPdmElementExtension>(node->extension.value());
        bin_ext.position = ext.position;
        bin_ext.icon = ext.icon;
        bin_ext.designation = ext.designation;
        bin_ext.letter_tag = ext.letter_tag;
        WiUpdatePdmNodeQuery updateQuery(*node);
        updateQuery.updateData = false;
        updateQuery.extension = toJson(bin_ext);
        updateQuery.updateExtension = true;
        updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return std::nullopt;

        return res;
    }

    std::optional<WiSemanticResult> PdmService::deleteWithDescendants(
            std::size_t initiatingService,
            const SemanticsHierarchyNode& desc,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        if(!desc.descendants.empty())
        {
            for(const auto& desc: desc.descendants)
            {
                deleteWithDescendants(initiatingService, desc, sessionPtr, ec, yield, mctx);
                if(ec) return std::nullopt;
            }

            auto parent = parentSemantic(desc.semantic, ec);
            if(ec) return std::nullopt;

            copyAllDescendantsTo(initiatingService, desc.semantic, parent, sessionPtr, ec, yield, mctx);
            if(ec) return std::nullopt;
        }

        auto res=deleteElementToBinInternal(initiatingService,desc.semantic,sessionPtr,ec,yield,mctx);
        return res;
    }

    std::optional<WiSemanticsResult> PdmService::deleteElementsToBin(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        std::set<std::string> parent_semantics;

        WiSemanticsResult result;
        for(const auto &semantic:query.semantics) {
            auto node = fetchRawNodeEntity(initiatingService,semantic,sessionPtr, ec, yield, mctx);
            if (ec) return std::nullopt;
            if (!node) {
                ec = make_error_code(error::node_not_found);
                return std::nullopt;
            }
        }

        SemanticsHierarchy semHierarchy{query.semantics};
        for(const auto &semHierarchyNode: semHierarchy.descendants) {

            std::optional<WiSemanticResult> res;

            res=deleteWithDescendants(initiatingService,semHierarchyNode,sessionPtr,ec,yield,mctx);

            if(ec || !res.has_value()){
                if(!ec){
                    ec = make_error_code(error::cant_create_element);
                }
                return std::nullopt;
            }
            result.semantics.push_back(res.value().semantic);
            boost::system::error_code tec;
            auto par = parentSemantic(semHierarchyNode.semantic, tec);
            if(!tec)
                parent_semantics.emplace(par);
        }

        for(const auto&parent_semantic:parent_semantics){
            WiRearrangePostionalIndecies rearrangeQuery;
            rearrangeQuery.prepend_mode = false;
            rearrangeQuery.destination = parent_semantic;
            rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        }

        if(ec) return std::nullopt;
        return result;
    }

    std::optional<WiSemanticsResult> PdmService::restoreElementsFromBin(
            std::size_t initiatingService,
            const WiRestoreElementsQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        WiSemanticsResult result;

        for (const auto& element : query.elements)
        {
            auto parent_semantic = parentSemantic(element.semantic, ec);
            if(ec) return std::nullopt;
            auto parent = fetchRawNodeEntity(initiatingService,parent_semantic,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;
            if(parent->role != PdmRoles::ProjectElementBin){
                ec = make_error_code(error::cant_restore_element);
                return std::nullopt;
            }
        }

        std::vector<std::shared_ptr<WiPdmRawNodeEntity>> nodes;
        nodes.reserve(query.elements.size());

        // construct and check nodes from query
        for(const auto& element: query.elements)
        {
            auto& node = nodes.emplace_back(fetchRawNodeEntity(initiatingService,element.semantic,sessionPtr,ec,yield,mctx));
            if(ec) return std::nullopt;

            if(!node->extension.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }

            if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::nullopt;
            }
            if(!node->entity->data.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
        }

        std::set<WiRearrangePostionalIndecies> rearrangeQuerys;

        for (size_t i = 0; i < nodes.size(); i++)
        {
            WiPdmBinElementExtension bin_ext = fromJson<WiPdmBinElementExtension>(nodes[i]->extension.value());

            WiMovePdmNodeQuery moveQuery;
            moveQuery.semantic = nodes[i]->semantic;
            bool prepend = false;
            std::string dest;
            if(!query.elements[i].destination.has_value()){
                prepend = bin_ext.prepend;
                dest = moveQuery.destination = bin_ext.location;
            }else{
                if(query.elements[i].prepend.has_value()){
                    prepend = query.elements[i].prepend.value();
                }
                dest = query.elements[i].destination.value();
                if(prepend) {
                    moveQuery.destination = parentSemantic(dest,ec);
                    if(ec) return std::nullopt;
                }else{
                    moveQuery.destination = dest;
                }
            }

            WiAddComponentWithData checkMoveQuery;
            checkMoveQuery.prepend = false;
            checkMoveQuery.destination = moveQuery.destination;

            checkElementDestination(initiatingService,checkMoveQuery,nodes[i]->role,nodes[i]->type,false,sessionPtr,ec,yield,mctx);
            if(ec) return std::nullopt;

            auto res = moveNodeInternal(initiatingService,moveQuery,sessionPtr, Filter::filterOn, ec,yield,mctx);
            if(ec || !res) return std::nullopt;
            //RESULT updates here
            result.semantics.push_back(res->semantic);

            // construct or update rearrange querys
            WiRearrangePostionalIndecies rearrangeQuery;
            rearrangeQuery.prepend_mode = prepend;
            rearrangeQuery.destination = dest;
            if (auto search = rearrangeQuerys.find(rearrangeQuery); search != rearrangeQuerys.end())
            {
                WiRearrangePostionalIndecies new_rearrangeQuery(*search);
                new_rearrangeQuery.elements.push_back(res->semantic);

                rearrangeQuerys.erase(search);
                rearrangeQuerys.insert(new_rearrangeQuery);
            }
            else
            {
                rearrangeQuery.elements.push_back(res->semantic);
                rearrangeQuerys.insert(rearrangeQuery);
            }
        }

        for (auto &rearrangeQuery : rearrangeQuerys)
        {
            // update
            rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
            if(ec) return std::nullopt;
        }

        //update extensions of restored nodes
        for (size_t i = 0; i < result.semantics.size(); i++)
        {
            auto new_node = fetchRawNodeEntity(initiatingService,result.semantics[i],sessionPtr,ec,yield,mctx);
            if(ec || !new_node) return std::nullopt;

            WiPdmBinElementExtension bin_ext = fromJson<WiPdmBinElementExtension>(nodes[i]->extension.value());

            mctx.addRestoredElementTrigger(new_node->semantic);
            WiUpdatePdmNodeQuery updateQuery(*new_node);
            WiPdmElementExtension ext = fromJson<WiPdmElementExtension>(new_node->extension.value());
            ext.bin_index = bin_ext.position;
            updateQuery.updateData = false;
            updateQuery.extension = toJson(ext);
            updateQuery.updateExtension = true;
            updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return std::nullopt;
        }

        return std::make_optional<WiSemanticsResult>(std::move(result));
    }

    std::optional<WiSemanticsResult> PdmService::copyElements(
            std::size_t initiatingService,
            const WiAddElementsToProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto res = copyElementsInternal(initiatingService, query, sessionPtr, ec, yield, mctx);
        if(ec || !res.has_value()){
            if(!ec){
                ec = make_error_code(error::cant_create_element);
            }
            return std::nullopt;
        }
        // now we need to build similar query
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = false;
        rearrangeQuery.destination = query.destination;
        rearrangeQuery.elements = std::move(res.value().semantics);
        if(query.prepend.has_value()){
            rearrangeQuery.prepend_mode = query.prepend.value();
        }
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        res.value().semantics = std::move(rearrangeQuery.elements);
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addContainerToProject(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        auto res = addContainerToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !res.has_value()){
            return std::nullopt;
        }
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = query.prepend;
        rearrangeQuery.destination = query.destination;
        rearrangeQuery.elements.push_back(res.value().semantic);
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addContainerToProjectInternal(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return std::nullopt;
        }
        boost::ignore_unused(initiatingService, query, sessionPtr, ec, yield, mctx);
        auto res = addBlankComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !res) return std::nullopt;
        WiUpdateComponentQuery transformQuery;
        transformQuery.letter_tag = query.letter_tag;
        transformQuery.designation=query.designation;
        transformQuery.role = PdmRoles::Container;
        transformQuery.semantic = res->semantic;
        transformQuery.header = query.header;
        transformQuery.description = query.description;
        transformQuery.data = query.data;
        transformQuery.isCreating = true;
        updateComponentData(initiatingService,transformQuery,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addContainerCopyToProject(
            std::size_t initiatingService,
            const WiAddComponentToProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return std::nullopt;
        }
        auto project = nodeNearestAncestor(initiatingService, destination, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec)
            return std::nullopt;
        auto project_source = nodeNearestAncestor(initiatingService, query.source, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec)
            return std::nullopt;
        if(project_source.semantic == project.semantic){
            auto source_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr, ec, yield, mctx);
            if(ec || !source_node){
                return std::nullopt;
            }
            if(source_node->role != PdmRoles::BlankComponent) {
                if (!source_node->entity.has_value()) {
                    ec = make_error_code(error::container_invalid);
                    return std::nullopt;
                }
                if (!source_node->entity.value().data.has_value()) {
                    ec = make_error_code(error::container_invalid);
                    return std::nullopt;
                }
                if (source_node->role != PdmRoles::Container) {
                    ec = make_error_code(error::invalid_node_role);
                    return std::nullopt;
                }
            }
            WiNewPdmNodeQuery addQuery;
            addQuery.parent = destination;
            addQuery.role = source_node->role;
            addQuery.type = source_node->type;
            addQuery.header = source_node->header;
            addQuery.description = source_node->description;
            addQuery.extension = source_node->extension;
            addQuery.data = source_node->entity->data;
            auto result = WiSemanticResult{addNewNodeInternal(initiatingService, addQuery, sessionPtr, Filter::filterOn, ec, yield, mctx)};
            if(ec)
                return std::nullopt;
            // add all descendandts
            copyAllDescendantsTo(initiatingService, query.source, result.semantic, sessionPtr, ec, yield, mctx);
            if(ec)
                return std::nullopt;
            return result;
        }
        else{
            //todo: implement copy from another project
            ec = make_error_code(error::entities_from_different_projects);
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<WiSemanticResult> PdmService::addBlankComponentToProject(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        checkElementDestination(initiatingService,
                                query,
                                PdmRoles::BlankComponent,
                                std::nullopt,
                                false,
                                sessionPtr,
                                ec,
                                yield,
                                mctx);
        if(ec) return std::nullopt;
        return addBlankComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
    }

    std::optional<WiSemanticResult> PdmService::addComponentToProjectInternal(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return std::nullopt;
        }
        boost::ignore_unused(initiatingService, query, sessionPtr, ec, yield, mctx);
        auto res = addBlankComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !res) return std::nullopt;
        WiUpdateComponentQuery transformQuery;
        transformQuery.letter_tag = query.letter_tag;
        transformQuery.designation=query.designation;
        transformQuery.role = PdmRoles::ElectricComponent;
        transformQuery.semantic = res->semantic;
        transformQuery.header = query.header;
        transformQuery.description = query.description;
        transformQuery.data = query.data;
        transformQuery.isCreating = true;
        updateComponentData(initiatingService,transformQuery,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;
        return res;
    }
    std::optional<WiSemanticResult> PdmService::addComponentToProject(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        checkElementDestination(initiatingService,
                                query,
                                PdmRoles::BlankComponent,
                                std::nullopt,
                                false,
                                sessionPtr,
                                ec,
                                yield,
                                mctx);
        if(ec) return std::nullopt;
        return addBlankComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
    }

    std::optional<WiSemanticResult> PdmService::addBlankComponentToProjectInternal(
        std::size_t initiatingService,
        const WiAddComponentWithData &query,
        const std::shared_ptr<IWiSession> sessionPtr,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return std::nullopt;
        }
        WiNewPdmNodeQuery addQuery;
        {
            WiPdmElementExtension ext;
            ext.icon = H_TO_S(StringHelper::BlankComponentIconSemantic);
            addQuery.extension = toJson(ext);
        }
        addQuery.parent = destination;
        addQuery.role = PdmRoles::BlankComponent;
        addQuery.type = std::nullopt;
        bool default_info = true;
        if(query.header.has_value()) {
            default_info = false;
            addQuery.header = query.header.value();
        }
        if(query.description.has_value()) {
            default_info = false;
            addQuery.description = query.description.value();
        }
        if(default_info){
            addQuery.header = getDefaultPdmBlankComponentHeader();
            addQuery.description = getDefaultPdmBlankComponentDescription();
        }
        addQuery.data = std::nullopt;
        return WiSemanticResult{addNewNodeInternal(initiatingService, addQuery, sessionPtr, Filter::filterOn, ec, yield, mctx)};
    }

    std::optional<WiSemanticsResult> PdmService::addElementsToProject(
            std::size_t initiatingService,
            const WiAddComponentsWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        auto res = addElementsToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !res.has_value()){
            return std::nullopt;
        }
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = query.prepend;
        rearrangeQuery.destination = query.destination;
        rearrangeQuery.elements = res.value().semantics;
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        return res;
    }

    std::optional<WiSemanticsResult> PdmService::addElementsToProjectInternal(
            std::size_t initiatingService,
            const WiAddComponentsWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(query.count <= 0) {
            ec = make_error_code(error::no_work_to_do);
            return std::nullopt;
        }
        switch(query.role){
            case PdmRoles::ProxyComponent:
                break;
            case PdmRoles::Container:
                break;
            case PdmRoles::ElectricComponent:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return std::nullopt;
        }
        WiSemanticsResult res;
        WiAddComponentWithData cQuery(query);
        for(std::uint64_t i = 0; i < query.count; i++){
            std::optional<WiSemanticResult> el;
            switch(query.role){
                case PdmRoles::ProxyComponent:
                    el = addProxyComponentToProjectInternal(initiatingService,cQuery,sessionPtr,ec,yield,mctx);
                    break;
                case PdmRoles::Container:
                    el = addContainerToProjectInternal(initiatingService,cQuery,sessionPtr,ec,yield,mctx);
                    break;
                case PdmRoles::ElectricComponent:
                    el = addComponentToProjectInternal(initiatingService,cQuery,sessionPtr,ec,yield,mctx);
                    break;
                default:
                    ec = make_error_code(error::invalid_node_role);
                    return std::nullopt;
            }
            if(ec) return std::nullopt;
            if(el.has_value())
            res.semantics.push_back(el.value().semantic);
        }
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addProxyComponentToProject(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto res = addProxyComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !res.has_value()){
            return std::nullopt;
        }
        WiRearrangePostionalIndecies rearrangeQuery;
        rearrangeQuery.prepend_mode = query.prepend;
        rearrangeQuery.destination = query.destination;
        rearrangeQuery.elements.push_back(res.value().semantic);
        rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        return res;
    }

    std::optional<WiSemanticResult> PdmService::addProxyComponentToProjectInternal(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        checkElementDestination(initiatingService,
                                query,
                                PdmRoles::ProxyComponent,
                                std::nullopt,
                                false,
                                sessionPtr,
                                ec,
                                yield,
                                mctx);
        if(ec) return std::nullopt;
        WiUpdateComponentQuery transformQuery;
        transformQuery.role = PdmRoles::ProxyComponent;
        auto node_sem = addBlankComponentToProjectInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
        if(ec || !node_sem) {
            return std::nullopt;
        }
        transformQuery.semantic = node_sem->semantic;
        transformQuery.data = std::make_optional<WiPdmElementData>();

        transformQuery.data->variables = query.data.variables;
        transformQuery.header = query.header;
        transformQuery.description = query.description;
        transformQuery.letter_tag = query.letter_tag;
        transformQuery.designation = query.designation;
        transformQuery.isCreating = true;

        updateComponentData(initiatingService,transformQuery,sessionPtr,ec,yield,mctx);
        if(ec) return std::nullopt;
        return node_sem;
    }

    std::optional<WiSemanticResult> PdmService::addComponentCopyToProject(
            std::size_t initiatingService,
            const WiAddComponentToProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return std::nullopt;
        }
        // find project parent
        auto project = nodeNearestAncestor(initiatingService, destination, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        auto project_def = nodeNearestAncestor(initiatingService, query.source, PdmRoles::Project, 1, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        if(project.semantic != project_def.semantic){
            ec = make_error_code(error::entities_from_different_projects);
            return std::nullopt;
        }
        auto source_node = fetchRawNodeEntity(initiatingService,query.source,sessionPtr, ec, yield, mctx);
        if(ec || !source_node){
            return std::nullopt;
        }
        if(!source_node->entity.has_value()){
            ec = make_error_code(error::component_invalid);
            return std::nullopt;
        }
        if(!source_node->entity.value().data.has_value()){
            ec = make_error_code(error::component_invalid);
            return std::nullopt;
        }
        WiNewPdmNodeQuery addQuery;
        addQuery.parent = destination;
        addQuery.role = source_node->role;
        addQuery.type = source_node->type;
        addQuery.header = source_node->header;
        addQuery.description = source_node->description;
        addQuery.extension = source_node->extension;
        addQuery.data = source_node->entity->data;
        return WiSemanticResult{addNewNodeInternal(initiatingService, addQuery, sessionPtr, Filter::filterOn, ec, yield, mctx)};
    }

    std::optional<WiSemanticResult> PdmService::addComponentDefinitionToProject(
            std::size_t initiatingService,
            const WiAddComponentToProject &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        // todo: add all subcategories too.
        GUARD_PDM_METHOD();
        // find project settings descendant
        auto componentsettings = nodeNearestDescendant(initiatingService, query.destination,
                                                       PdmRoles::ProjectSettingsComponents, 1, sessionPtr, ec,
                                                       yield, mctx);
        if (ec) return std::nullopt;
        // fetch all component settings
        WiPdmRawNodeEntity::Container refs;
        fetchRawNodesEntity(initiatingService,componentsettings.semantic, refs,sessionPtr, ec, yield, mctx);
        if (ec) return std::nullopt;
        // find a component ref with same source
        auto it = std::find_if(refs.begin(), refs.end(), [&query](const auto &item) {
            if (item.role != PdmRoles::ProjectSettingsComponentsRef) {
                // thats an error, project::settings::components should not contain
                // any other nodes other than component refs
                return false;
            }
            if (item.entity.has_value()) {
                if (item.entity.value().data.has_value()) {
                    if (item.entity.value().data.value().is_object()) {
                        if (item.entity.value().data.value().contains("ster")) {
                            const nlohmann::json & ster = item.entity.value().data.value()["ster"];
                            if (ster.is_object()) {
                                if(ster.contains("source")) {
                                    const nlohmann::json & source = ster["source"];
                                    if(source.is_string())
                                        return  source == query.source;
                                }
                            }
                        }
                    }
                }
                // error - component ref is not in the right format
                return false;
            }
            return false;
        });
        // if found then add a new component with same data, if not then create new setting
        if (it == refs.end()) {
            // create new setting
            auto newRef = createComponentRefInternal(
                    initiatingService,
                    componentsettings.semantic,
                    query.source,
                    sessionPtr, ec, yield, mctx);
            if (ec || !newRef.has_value()) return std::nullopt;
            return WiSemanticResult{newRef.value().semantic};
        } else {
            // add element with an existing setting
            return WiSemanticResult{it->semantic};
        }
    }

    std::optional<WiPdmComponentData> PdmService::fetchComponentData(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        // fetch only specific data for element
        // Blank - none (done)
        // Proxy, Container - variables (done)
        // Component - krr,ster,variables (done)
        boost::ignore_unused(initiatingService, sessionPtr);
        auto node = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        if(!node){
            ec = make_error_code(error::node_not_found);
            return std::nullopt;
        }
        std::optional<long double> timespan;
        if(node->role == PdmRoles::ProxyComponent
           || node->role == PdmRoles::ElectricComponent){
            boost::system::error_code tec;
            auto prod_node = nodeNearestAncestor(initiatingService,query.semantic,PdmRoles::Product,1,sessionPtr,tec,yield,mctx);
            if(!tec) {
                auto prod_node_ = fetchRawNodeEntity(initiatingService,prod_node.semantic,sessionPtr, tec, yield, mctx);
                if (!tec) {
                    WiPdmElementData prod_data;
                    if(prod_node_->entity.has_value()){
                        if(prod_node_->entity->data.has_value()){
                            prod_data = fromJson<decltype(prod_data)>(prod_node_->entity->data.value());
                        }
                    }
                    if (prod_data.ster.has_value()) {
                        auto it = prod_data.ster->data.find("expected_life_time");
                        if (it != prod_data.ster->data.end()) {
                            auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                            if (gen_val != nullptr) {
                                auto val = std::get_if<std::optional<long double>>(gen_val);
                                if (val != nullptr) {
                                    timespan = *val;
                                }
                            }
                        }
                    }
                }
            }
        }
        if(node->role == PdmRoles::ElectricComponent) {
            if(!node->entity.has_value()) {
                ec = make_error_code(error::component_invalid);
                return std::nullopt;
            }
            if(!node->entity.value().data.has_value()){
                ec = make_error_code(error::component_invalid);
                return std::nullopt;
            }
            auto data = fromJson<WiPdmElementData>(node->entity.value().data.value());
            WiPdmComponentData ret_data;

            if(data.variables.has_value()){
                if(data.variables->failure_rate.has_value()){
                    data.variables->failure_rate.value() *= 1.e6L;
                }
            }
            ret_data.handbook = data.handbook;
            ret_data.item = data.item;
            ret_data.type = data.type;
            ret_data.variables = std::move(data.variables);

            ret_data.functional_units = std::move(data.functional_units);
            ret_data.parameters = data.parameters;
            ret_data.product_time = timespan;

            return std::make_optional(std::move(ret_data));
        }
        else if(node->role == PdmRoles::ProjectSettingsComponentsRef){
            if (!node->entity.has_value()) {
                ec = make_error_code(error::component_ref_invalid);
                return std::nullopt;
            }
            if (!node->entity.value().data.has_value()) {
                ec = make_error_code(error::component_ref_invalid);
                return std::nullopt;
            }
            WiPdmComponentData ret_data;
            ret_data.krr = std::make_optional<WiMdmStereotypeWithData>();
            ret_data.ster = std::make_optional<WiMdmStereotypeWithData>();
            auto ref_data = fromJson<WiPdmComponentRefData>(node->entity.value().data.value());
            ret_data.ster->data = std::move(ref_data.data);
            ret_data.ster->stereotype = std::move(ref_data.ster);
            ret_data.krr->stereotype = std::move(ref_data.krr);
            ret_data.krr->data = std::move(ref_data.env_data);
            return std::make_optional(std::move(ret_data));
        }
        else if(node->role == PdmRoles::ProxyComponent){
            if(!node->entity.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            if(!node->entity.value().data.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            auto el_data = fromJson<WiPdmElementData>(node->entity.value().data.value());

            WiPdmComponentData ret_data;
            ret_data.product_time = timespan;
            ret_data.functional_units = std::move(el_data.functional_units);
            if(el_data.variables.has_value()){
                if(el_data.variables->failure_rate.has_value()){
                    el_data.variables->failure_rate.value() *= 1.e6;
                }
            }
            ret_data.variables = std::move(el_data.variables);
            return std::make_optional(std::move(ret_data));
        }else if( node->role == PdmRoles::Container ){
            if(!node->entity.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            if(!node->entity.value().data.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            auto el_data = fromJson<WiPdmElementData>(node->entity.value().data.value());

            WiPdmComponentData ret_data;
            ret_data.functional_units = std::move(el_data.functional_units);
            if(el_data.variables.has_value()){
                if(el_data.variables->failure_rate.has_value()){
                    el_data.variables->failure_rate.value() *= 1.e6;
                }
            }
            ret_data.variables = std::move(el_data.variables);
            ret_data.ref = el_data.ref;
            return std::make_optional(std::move(ret_data));
        }
        else if(node->role == PdmRoles::BlankComponent){
            return std::make_optional<WiPdmComponentData>();
        } else if(node->role == PdmRoles::RbdSchema){ // fixme: КОСТЫЛЬ
            WiPdmComponentData ret_data;

            if(!node->entity.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            if(!node->entity->data.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            WiPdmComponentData data = fromJson<decltype(data)>(node->entity->data.value());
            if(data.variables.has_value()){
                if(data.variables->failure_rate.has_value()){
                    data.variables->failure_rate.value() *= 1.e6;
                }
            }
            ret_data.variables = std::move(data.variables);
            return std::make_optional(std::move(ret_data));
        } else if(node->role == PdmRoles::Product){ // fixme: КОСТЫЛЬ
            WiPdmComponentData ret_data;

            if(!node->entity.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            if(!node->entity->data.has_value()){
                ec = make_error_code(error::element_invalid);
                return std::nullopt;
            }
            WiPdmComponentData data = fromJson<decltype(data)>(node->entity->data.value());
            if(data.variables.has_value()){
                if(data.variables->failure_rate.has_value()){
                    data.variables->failure_rate.value() *= 1.e6;
                }
            }
            ret_data.variables = std::move(data.variables);
            ret_data.ster = std::make_optional<WiMdmStereotypeWithData>();
            if(data.ster.has_value()){
                ret_data.ster->data = data.ster->data;
                ret_data.ster->stereotype.stereotype = WiStereotype({
                   WiStereotypeElement{std::nullopt,0,WiStereotypeParameter{
                       "expected_life_time"
                   }},
                   WiStereotypeElement{std::nullopt,0,WiStereotypeParameter{
                       "MDM::SETTINGS::PARAMETERS::ambient_temperature"
                   }}
                });
            }

            return std::make_optional(std::move(ret_data));
        }
        else{
            ec = make_error_code(error::invalid_node_role);
            return std::nullopt;
        }
    }

    std::optional<std::vector<WiHandbookMetadata>> PdmService::getHandbooksMetadata(
            std::size_t initiatingService,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::ignore_unused(initiatingService, sessionPtr);
        GUARD_PDM_METHOD();
        std::vector<WiHandbookMetadata> ret;
        auto codes = ReliabilityHandbookManagerSvc.getRegisteredHandbooks();
        for(const auto & code: codes) {
            ec = boost::system::error_code();
            auto handbook = ReliabilityHandbookManagerSvc.getHandbook(code);
            if (!handbook) continue;
            WiHandbookMetadata metadata;
            {
                //handbook metadata and global parameter overrides
                auto name = handbook->getName(ec);
                if (ec || !name.has_value()) continue;
                metadata.name = name.value();
                metadata.code = code;
                auto build = handbook->getBuild(ec);
                if (ec || !build.has_value()) continue;
                metadata.build = build.value();
                auto global_overrides = handbook->getParameterOverrides(ec);
                if (ec || !global_overrides.has_value()) continue;
                metadata.parameter_overrides = global_overrides.value();
                auto parameter_translates = handbook->getParameterTranslates(ec);
                if(ec || !parameter_translates.has_value()) continue;
                metadata.parameter_translates = parameter_translates.value();
            }
            {
                // type metadata
                auto items = handbook->getSupportedClasses(ec);
                if(ec || !items.has_value()) continue;

                for(const auto &item:items.value()){
                    ec = boost::system::error_code();
                    auto types = handbook->getSupportedTypes(item,ec);
                    if(ec || !types.has_value()) break;

                    WiHandbookClassMetadata class_metadata;
                    auto class_name = handbook->getClassName(item,ec);
                    if(ec || !class_name.has_value()) break;
                    class_metadata.name = class_name.value();

                    for(const auto &type:types.value()){
                        ec = boost::system::error_code();
                        auto handbook_type = handbook->getType(item, type, ec);
                        if(ec || !handbook_type) continue;
                        auto md = handbook_type->getMetadata(ec);
                        if(ec || !md.has_value()) continue;
                        auto type_name = handbook_type->getName(ec);
                        if(ec || !type_name.has_value()) continue;
                        WiHandbookTypeMetadata type_metadata;
                        type_metadata.name = type_name.value();
                        type_metadata.meta = md.value();
                        class_metadata.types.insert(std::make_pair(type, type_metadata));
                    }
                    ec = boost::system::error_code();

                    metadata.items.insert(std::pair(item,class_metadata));
                }
                if(ec) continue;
            }
            ret.push_back(metadata);
        }
        return ret;
    }

    void PdmService::updateComponentData(
            std::size_t initiatingService,
            const WiUpdateComponentQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

#ifdef DEBUG
        if(query.data) {
            if(query.data->parameters.has_value()) {
                reliability::handbooks::printDiagnostic(query.data->parameters.value());
            }
        }

#endif

        // update component data by given semantics
        auto node = fetchRawNodeEntity(initiatingService, query.semantic, sessionPtr, ec, yield, mctx);

        updateComponentQueryCheck(query, node, ec);
        if(ec) return;

        WiUpdatePdmNodeQuery updateQuery(*node);
        updateQuery.updateData = false;

        if(query.role.has_value() &&
           node->role != PdmRoles::Product &&
           node->role != PdmRoles::RbdSchema)
        {
            initiateRecalculation(initiatingService, sessionPtr, node->semantic, ec, yield, mctx);
            if(ec) return;
            //явно задаём role для node
            node->role = query.role.value();

            transformElement_impl(initiatingService, query, updateQuery, node,
                                  sessionPtr, ec, yield, mctx);
            if(ec) return;
            updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return;
            std::string temp = query.semantic;
            fetchRawNode(temp, ec, yield, mctx,true);
            if(ec) return;
            node = fetchRawNodeEntity(initiatingService,query.semantic,sessionPtr, ec, yield, mctx);
            if(ec) return;
            updateQuery = WiUpdatePdmNodeQuery(*node);
            updateQuery.updateData = false;
        }

        if (ec) return;

        std::list<WiUpdatePdmNodeQuery> FUupdates;
        std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries{query.semantic,FUupdates};

        updateComponentData_impl(initiatingService,query, updateQuery, node, FU_updQueries, sessionPtr, ec, yield, mctx);

        if(ec) return;

        for(auto& FUupdate: FUupdates)
        {
            updateNode(initiatingService, FUupdate, sessionPtr, Filter::filterOn, ec, yield, mctx);
            if(ec) return;
        }

        
        updateNode(initiatingService, updateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if(ec) return;

        if (node->extension.has_value()) {
            auto nodeExtension =  fromJson<WiPdmElementExtension>(node->extension.value());
            for (const auto &[scheme, blocks]: nodeExtension.rbd_block_refs) {
                for (const auto &block: blocks) {
                    fillRbdBlockVars(initiatingService, node->semantic, block, sessionPtr, ec, yield, mctx);
                }
            }
        }

        if (query.failure_types.has_value())
            changeElementFailureTypes(initiatingService, node->semantic, query.failure_types.value(), sessionPtr, ec, yield, mctx);
    }

    void PdmService::updateComponentQueryCheck(const WiUpdateComponentQuery &query,
                                       std::shared_ptr<WiPdmRawNodeEntity> node,
                                       boost::system::error_code &ec) const noexcept(true)
    {
        if(ec || !node) return;

        if(query.role.has_value())
        {
            switch(query.role.value()){
                case PdmRoles::ProxyComponent:
                case PdmRoles::Container:
                case PdmRoles::ElectricComponent:
                case PdmRoles::BlankComponent:
                    if(node->role == PdmRoles::Product || node->role == PdmRoles::RbdSchema) {
                        ec = make_error_code(error::invalid_input_data);
                        return;
                    }
                    break;
                default:
                    ec = make_error_code(error::invalid_node_role);
                    return;
            }
        }

        if(node->role)
        {
            switch(node->role){
                case PdmRoles::ProxyComponent:
                case PdmRoles::Container:
                case PdmRoles::ElectricComponent:
                case PdmRoles::BlankComponent:
                case PdmRoles::Product:
                case PdmRoles::RbdSchema:
                    break;
                default:
                    ec = make_error_code(error::invalid_node_role);
                    return;
            }
        }
        else
        {
            ec = make_error_code(error::invalid_node_role);
            return;
        }

        if (node->role == PdmRoles::ProxyComponent ||
            node->role == PdmRoles::Container ||
            node->role == PdmRoles::ElectricComponent ||
            node->role == PdmRoles::Product ||
            node->role == PdmRoles::RbdSchema)
        {
            if (!node->entity.has_value()) {
                ec = make_error_code(error::element_invalid);
                return;
            }
            if (!node->entity->data.has_value()) {
                ec = make_error_code(error::element_invalid);
                return;
            }

        }
    }

    void PdmService::updateComponentData_impl(
                            std::size_t initiatingService,
                            const WiUpdateComponentQuery& query,
                            WiUpdatePdmNodeQuery& updateQuery, //out
                            std::shared_ptr<WiPdmRawNodeEntity> node,
                            std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
                            const std::shared_ptr<IWiSession> sessionPtr,
                            boost::system::error_code &ec,
                            const net::yield_context &yield,
                            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        //todo: update only specific data for element
        // Blank - none
        // Proxy - variables
        // Component - krr, ster, ref(if not a link to mdm component)
        // update header/description for all of the elements
        // update component data by given semantics
        if (ec || !node) return;
        bool infoUpdated = false;
        if (query.header.has_value()) {
            updateQuery.header = query.header.value();
            infoUpdated = true;
        }
        if (query.description.has_value()) {
            updateQuery.description = query.description.value();
            infoUpdated = true;
        }
        if (!updateQuery.header.empty() || !updateQuery.description.empty()) {
            infoUpdated = true;
        }
        WiPdmElementExtension ext;
        WiPdmElementData el_data;
        bool updated = false;

        WiPdmRbdExtension rbd_ext;
        bool rbd_updated = false;

        if (node->role == PdmRoles::ProxyComponent ||
            node->role == PdmRoles::Container ||
            node->role == PdmRoles::ElectricComponent) {

            auto prod_node = nodeNearestAncestor(initiatingService, node->semantic, PdmRoles::Product, 1, sessionPtr, ec, yield, mctx);
            if (ec) return;

            if (node->extension.has_value()) {
                ext = fromJson<WiPdmElementExtension>(node->extension.value());
            }
            if (node->entity.has_value()) {
                if (node->entity.value().data.has_value()) {
                    el_data = fromJson<WiPdmElementData>(node->entity.value().data.value());
                }
            }
            std::optional<long double> timespan;
            boost::system::error_code tec;
            auto prod_node_ = fetchRawNodeEntity(initiatingService,prod_node.semantic,sessionPtr, ec, yield, mctx);
            if (!tec) {
                WiPdmElementData prod_data;
                if(prod_node_->entity.has_value()){
                    if(prod_node_->entity->data.has_value()){
                        prod_data = fromJson<decltype(prod_data)>(prod_node_->entity->data.value());
                    }
                }
                if (prod_data.ster.has_value()) {
                    auto it = prod_data.ster->data.find("expected_life_time");
                    if (it != prod_data.ster->data.end()) {
                        auto gen_val = std::get_if<WiValueGeneral>(&it->second.value);
                        if (gen_val != nullptr) {
                            auto val = std::get_if<std::optional<long double>>(gen_val);
                            if (val != nullptr) {
                                timespan = *val;
                            }
                        }
                    }
                }
            }
            //todo: обновлять так же и параметры, и свойства в случае если это возможно(если компонент созданный вручную например)
            if (query.data.has_value()) {
                // todo: write a function to apply changes to stereotype data map
//                if (query.data->krr.has_value() && node->role == PdmRoles::ElectricComponent) {
//                    auto &env_data = el_data.krr->data;
//                    for (const auto &pair: query.data->krr->data) {
//                        if (!env_data.count(pair.first)) {
//                            ec = make_error_code(error::trying_to_set_missing_parameter);
//                            return;
//                        }
//
//                        auto &par = env_data.at(pair.first);
//                        if (!par.overridable) {
//                            ec = make_error_code(error::trying_to_set_non_overridable_parameter);
//                            return;
//                        }
//
//                        if (!core::validators::WiValueValidator::validate(
//                                pair.first,
//                                pair.second.value,
//                                mdm::internal::DefaultParameterProvider(yield, ctx),
//                                mdm::internal::DefaultUnitProvider(yield, ctx),
//                                ec)) {
//                            if (!ec) {
//                                ec = make_error_code(error::trying_to_set_invalid_value);
//                            }
//                        }
//                        if (ec) {
//                            return;
//                        }
//                        par.value = pair.second.value;
//                        par.defined = node->semantic;
//                        updated = true;
//                    };
//                }

//                WI_LOG_DEBUG()<<"update component query: " << toJson(query).dump();
                if ((query.data->parameters.has_value()
                    || query.data->handbook.has_value()
                    || query.data->item.has_value()
                    || query.data->type.has_value()
                    )&& node->role == PdmRoles::ElectricComponent){
                    handbooks::internal::HandbookWrapperPtr handbook_ptr;

                    // set handbook reference - checking that the handbook is connected
                    bool parameters_updated = false;
                    if(query.data->handbook.has_value()){
                        auto handbook = query.data->handbook.value();
                        handbook_ptr = ReliabilityHandbookManagerSvc.getHandbook(handbook.code, handbook.build);
                        if(!handbook_ptr) {
                            //todo:error
                        }
                        el_data.handbook = handbook;
                        parameters_updated = true;
                    }else{
                        if(el_data.handbook.has_value()) {
                            handbook_ptr = ReliabilityHandbookManagerSvc
                                .getHandbook(el_data.handbook->code, el_data.handbook->build);
                        }
                    }

                    // set the types and classes if provided
                    if(query.data->item.has_value()){
                        el_data.item = query.data->item;
                        parameters_updated = true;
                    }
                    if(query.data->type.has_value()){
                        el_data.type = query.data->type;
                        parameters_updated = true;
                    }

                    // set the parameters, despite the fact that the handbook is available or not
                    if(!el_data.parameters.has_value()){
                        el_data.parameters = reliability::handbooks::ParameterMap();
                    }
                    auto combine_parameters = [](const auto &input, auto &output){
                        for(auto it=input.begin(),end = input.end(); it != end;++it){
                            output.erase(it->first);
                            if(it->second.value.has_value()){
                                output.insert(*it);
                            }
                        }
                    };
                    // combine parameters
                    if(query.data->parameters.has_value()){
                        combine_parameters(query.data->parameters.value(), el_data.parameters.value());
                        if(!query.data->parameters.value().empty()){
                            parameters_updated = true;
                        }
                    }
                    if(parameters_updated){
                        //recalculate parameters
                        WiPdmElementVariables vars;
                        if(el_data.item.has_value() && el_data.type.has_value() && handbook_ptr) {
                            auto componentClass = el_data.item.value();
                            auto type = el_data.type.value();
                            boost::system::error_code tec;
                            auto result_parameters = handbook_ptr->calculateComponent(componentClass,type,el_data.parameters.value(),tec);
                            if(tec)
                            {
                                ec = tec;
                                return;
                            }
                            if(!tec && result_parameters.has_value()){
                                combine_parameters(result_parameters.value(),el_data.parameters.value());
                            }
                            auto FR = reliability::handbooks::parameters::ParameterMapWrapper(el_data.parameters.value()).at<reliability::handbooks::parameters::types::failure_rate>();
                            if(FR.has_value()){
                                vars.failure_rate = FR.value();
                            }
                        }
                        fillAllVars(vars,timespan,ec);
                        el_data.variables = vars;
                    }
                }
                if (query.data->variables.has_value() && node->role == PdmRoles::ProxyComponent) {
                    // todo: also calculate (or calculate from) probability (outside of MVP)
                    WiPdmElementVariables vars;
                    vars = query.data->variables.value();
                    if(!vars.failure_rate.has_value() and !vars.MTBF.has_value() and
                        !(vars.set_reliability.has_value() == vars.set_reliability_time.has_value() and vars.set_reliability.has_value())){
                        ec = make_error_code(error::invalid_input_data);
                        return;
                    }
                    if(vars.failure_rate.has_value()){
                        vars.failure_rate = vars.failure_rate.value() / 1.e6L;
                    }
                    fillAllVars(vars,timespan,ec);
                    if(ec) return;
                    el_data.variables = vars;
                    updated = true;
                }

                if(query.data->functional_units.has_value())
                {
                    bool FUset = updateElementFunctionalUnits(
                                                initiatingService,
                                                prod_node.semantic,
                                                el_data,query.data->functional_units.value(),
                                                FU_updQueries,
                                                sessionPtr,ec,yield,mctx);
                    if(ec) return;
                    updated = updated || FUset;
                    if (ec) return;
                }
            }
            if (query.designation.has_value()) {
                ext.designation = query.designation.value();
                updated = true;
            }
            WiPdmElementData prod_data;
            auto prod = fetchRawNodeEntity(initiatingService,prod_node.semantic,sessionPtr, ec, yield, mctx);
            if(ec) return;
            WiUpdatePdmNodeQuery updateQueryProduct(*prod);
            if (prod->entity.has_value()) {
                if (prod->entity.value().data.has_value()) {
                    prod_data = fromJson<WiPdmElementData>(prod->entity.value().data.value());
                }
            }
            bool marking_bool = false;
            if (query.letter_tag.has_value()) {
                std::string new_data = query.letter_tag.value();
                std::string old_data = ext.letter_tag;
                if (query.isCreating) {
                    ext.letter_tag = positional_index_cache_conversion(new_data, old_data, prod_data, ec, marking_bool);
                    if (ec) return;
                }
                else {
                    auto old_data_parsed = positional_parse_data(ext.letter_tag);
                    positional_remove_old_from_prod_data(old_data_parsed.first,old_data_parsed.second,prod_data,ec,marking_bool);
                    ext.letter_tag = new_data;
                    if (ec) return;
                }
            } else {
                auto old_data_parsed = positional_parse_data(ext.letter_tag);
                positional_remove_old_from_prod_data(old_data_parsed.first,old_data_parsed.second,prod_data,ec,marking_bool);
                if (ec) return;
                ext.letter_tag = "";
            }
            if (marking_bool) {
                updated = true;
                updateQueryProduct.data = toJson(prod_data);
                updateQueryProduct.updateData = true;
                updateNode(initiatingService, updateQueryProduct, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if (ec) return;
            }
        }
        else if(node->role == PdmRoles::Product){
            if (node->extension.has_value()) {
                ext = fromJson<WiPdmElementExtension>(node->extension.value());
            }
            if (node->entity.has_value()) {
                if (node->entity.value().data.has_value()) {
                    el_data = fromJson<WiPdmElementData>(node->entity.value().data.value());
                }
            }
            if(!query.data.has_value()) return;
            if (query.data->ster.has_value() && node->role == PdmRoles::Product) {
                auto &env_data = el_data.ster->data;
                bool params_updated = true;
                for (const auto &pair: query.data->ster->data) {
                    if (!env_data.count(pair.first)) {
                        ec = make_error_code(error::trying_to_set_missing_parameter);
                        return;
                    }

                    auto &par = env_data.at(pair.first);
                    if (!par.overridable) {
                        ec = make_error_code(error::trying_to_set_non_overridable_parameter);
                        return;
                    }

                    if (!core::validators::WiValueValidator::validate(
                        pair.first,
                        pair.second.value,
                        mdm::internal::DefaultParameterProvider(yield, ctx),
                        mdm::internal::DefaultUnitProvider(yield, ctx),
                        ec)) {
                        if (!ec) {
                            ec = make_error_code(error::trying_to_set_invalid_value);
                        }
                    }
                    if (ec) {
                        return;
                    }
                    par.value = pair.second.value;
                    par.defined = node->semantic;
                    params_updated = true;
                };
                if(params_updated){
                    updated = true;
                    mctx.addProductTrigger(node->semantic);
                }
            }
        }
        else if(node->role == PdmRoles::RbdSchema){
            if (node->extension.has_value()) {
                rbd_ext = fromJson<WiPdmRbdExtension>(node->extension.value());
            }

            if(query.expected_life_time.has_value())
            {
                rbd_ext.expected_life_time = std::stold(query.expected_life_time.value());
                rbd_updated = true;
            }
            else
            {
                ec = make_error_code(error::no_work_to_do);
                return;
            }

        }

        if (!infoUpdated && !updated) {
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        if (!ec && updated) {
            updateQuery.updateData = true;
            updateQuery.data = toJson(el_data);
            updateQuery.extension = toJson(ext);
            updateQuery.updateExtension = true;
        }
        if (!ec && rbd_updated) {
            updateQuery.extension = toJson(rbd_ext);
            updateQuery.updateExtension = true;
        }
        else if (node->role == PdmRoles::BlankComponent) {
            if (!infoUpdated) {
                ec = make_error_code(error::no_input_data);
                return;
            }
        }
        initiateRecalculation(initiatingService,sessionPtr,node->semantic,ec,yield,mctx);
    }

    void PdmService::transformElement_impl(
            std::size_t initiatingService,
            const WiUpdateComponentQuery& query,
            WiUpdatePdmNodeQuery& updateQuery, //out
            std::shared_ptr<WiPdmRawNodeEntity> node,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        boost::ignore_unused(initiatingService,sessionPtr,ec,yield,ctx);
        GUARD_PDM_METHOD();
        if(!node || ec) return;

//        // если пытаемся трансформировать в то же что уже есть - просто завершаем.
//        if(query.role == node->role){
//            return;
//        }

        WiPdmElementData data;
        WiPdmElementExtension extension;
        if(node->role != PdmRoles::BlankComponent)
        {
            if(node->entity.has_value())
            {
                if(node->entity.value().data.has_value())
                {
                    data = fromJson<WiPdmElementData>(node->entity.value().data.value());
                }
            }
        }

        if(!node->extension.has_value())
        {
            ec = make_error_code(error::element_invalid);
            return;
        }
        extension = fromJson<WiPdmElementExtension>(node->extension.value());
        updateQuery.type = std::nullopt;
        updateQuery.updateType = true;

        bool reset_info = true;
        if(query.header.has_value()){
            reset_info = false;
            updateQuery.header = query.header.value();
        }else{
            updateQuery.header = node->header;
        }

        if(query.description.has_value()){
            reset_info = false;
            updateQuery.description = query.description.value();
        }else{
            updateQuery.description = node->description;
        }


        if (query.role.value() == PdmRoles::ProxyComponent ||
            query.role.value() == PdmRoles::ElectricComponent)
        {
            if(!query.data.has_value()){
                ec = make_error_code(error::no_input_data);
                return;
            }
            if(query.role.value() == PdmRoles::ProxyComponent)
            {
                if(!query.data->variables.has_value()){
                    ec = make_error_code(error::no_input_data);
                    return;
                }
            }
        }

        if (reset_info)
        {
            switch(query.role.value())
            {
                case PdmRoles::ProxyComponent:
                    updateQuery.header = getDefaultPdmProxyComponentHeader();
                    updateQuery.description = getDefaultPdmProxyComponentDescription();
                    break;
                case PdmRoles::BlankComponent:
                    updateQuery.header = getDefaultPdmBlankComponentHeader();
                    updateQuery.description = getDefaultPdmBlankComponentDescription();
                    break;
                case PdmRoles::ElectricComponent: {
                    updateQuery.header = getDefaultPdmComponentHeader();
                    updateQuery.description = getDefaultPdmComponentDescription();
                    if(query.data.has_value()){
                        auto &d = query.data.value();
                        if(d.handbook.has_value() and d.type.has_value() and d.item.has_value()){
                            auto hdbk = ReliabilityHandbookManagerSvc.getHandbook(d.handbook->code,d.handbook->build);
                            boost::system::error_code tec;
                            auto name = hdbk->getClassName(d.item.value(),tec);
                            if(name.has_value()){
                                auto desc = hdbk->getTypeName(d.item.value(),d.type.value(),tec);
                                if(desc.has_value()){
                                    updateQuery.header = name.value();
                                    updateQuery.description = desc.value();
                                }
                            }
                        }
                    }
                }
                    break;
                case PdmRoles::Container:
                    updateQuery.header = getDefaultPdmContainerHeader();
                    updateQuery.description = getDefaultPdmContainerDescription();
                default:
                    break;
            }
        }

        bool updatePositional = false;
        //todo: also assign positional if the old node was without it
        //todo: clear all links and dependecies of old role b4 reaplying role
        decltype(fetchRawNode(data.ref.value(), ec, yield, mctx)) ref_node;
        switch(query.role.value()){
            case PdmRoles::ProxyComponent:
                if(node->role == PdmRoles::BlankComponent){
                    updatePositional = true;
                }
                updateQuery.role = PdmRoles::ProxyComponent;
                updateQuery.type = std::nullopt;

                if(!query.data->variables->failure_rate.has_value()
                    && !query.data->variables->MTBF.has_value()
                    && !(query.data->variables->set_reliability.has_value() and query.data->variables->set_reliability_time.has_value())){
                    if(!data.variables.has_value()){
                        ec = make_error_code(error::no_input_data);
                        return;
                    }
                }else {
                    data.variables = query.data->variables;
                }

                // if there is no variables provided but element contains them already - just leave as is
                break;
            case PdmRoles::Container:
                if(node->role == PdmRoles::BlankComponent){
                    updatePositional = true;
                }
                updateQuery.role = PdmRoles::Container;

                extension.icon = "";
                node->role = PdmRoles::Container;
                data.variables = std::nullopt;
                // todo: add check that all descendants are allowed there
                checkDescendantsDestination(initiatingService,node,sessionPtr,ec,yield,mctx);
                if(ec) return;
                break;
            case PdmRoles::ElectricComponent:
                if(node->role == PdmRoles::BlankComponent){
                    updatePositional = true;
                }
                updateQuery.role = PdmRoles::ElectricComponent;
                updateQuery.type = std::nullopt;

                // todo: set by ref
                data.variables = std::nullopt;
                //todo: icon
                break;
            case PdmRoles::BlankComponent:
                updateQuery.role = PdmRoles::BlankComponent;
                updateQuery.type = std::nullopt;
                data.variables = std::nullopt;
                if(node->role != PdmRoles::BlankComponent) {
                    extension.position = "";
                    updatePositional = true;
                }
                extension.icon = H_TO_S(StringHelper::BlankComponentIconSemantic);
                break;

        }


        {
            WiAddComponentWithData checkQuery;
            checkQuery.destination = parentSemantic(query.semantic, ec);
            checkQuery.prepend = false;
            checkElementDestination(initiatingService,
                                    checkQuery,
                                    updateQuery.role,
                                    updateQuery.type,
                                    false,
                                    sessionPtr,
                                    ec,
                                    yield,
                                    mctx);
            if(ec) return;
        }
        updateQuery.updateRole = true;
        updateQuery.data = toJson(data);
        updateQuery.updateData = true;
        updateQuery.extension =  toJson(extension);
        updateQuery.updateExtension = true;

        if(updatePositional){
            WiRearrangePostionalIndecies rearrangeQuery;
            rearrangeQuery.prepend_mode = false;
            rearrangeQuery.destination = parentSemantic(query.semantic, ec);
            if(ec) return;
            if(query.role.value() != PdmRoles::BlankComponent){
                rearrangeQuery.elements.push_back(query.semantic);
            }
            rearrangePositionalIndecies(initiatingService, rearrangeQuery, sessionPtr, ec, yield, mctx);
            if(ec) return;
        }
    }


    std::optional<WiPdmNodeView::Container> PdmService::fetchProjectsView(
            std::size_t initiatingService,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        return fetchNodesView<WiPdmNodeView>(initiatingService, H_TO_S(StringHelper::PdmProjectsSemantics), language, sessionPtr, ec, yield, mctx);
    }


    std::optional<WiProjectServiceInfo> PdmService::fetchProjectServiceInfo(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){

        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        WiProjectServiceInfo info;
        std::vector<std::int64_t> roles = {
            PdmRoles::FunctionalUnit,
            PdmRoles::RbdSchema,
            PdmRoles::ElectricComponent,
            PdmRoles::Container,
            PdmRoles::ProxyComponent
        };


        DataAccessConst().fetchPdmNodeServiceInfo(info.element_count, semantic, roles, 0, mctx, ec, yield);// B
        if(ec) {
            return std::nullopt;
        }
        WiPdmRawNode trash = nodeNearestDescendant(initiatingService,semantic,PdmRoles::ProjectElementBin,0,sessionPtr,ec,yield,mctx);
        if(ec) {
            return std::nullopt;
        }

        WiNodeElementCount::Container bin_element_count;// C
        DataAccessConst().fetchPdmNodeServiceInfo(bin_element_count, trash.semantic, roles, 0, mctx, ec, yield);
        if(ec) {
            return std::nullopt;
        }
        std::for_each(begin(info.element_count), end(info.element_count), [&bin_element_count](WiNodeElementCount &el_info)
        {
            auto it = std::find_if(begin(bin_element_count),end(bin_element_count), [&el_info] (WiNodeElementCount el_info2)
            {
                if (el_info.role== el_info2.role and el_info.type== el_info2.type)
                {
                    return true;
                }
                else return false;
            });
            if(it !=end(bin_element_count)){
                el_info.count = el_info.count-it->count;
            }
            if (el_info.count==0)
            {
                bin_element_count.erase(it);
            }
        });

        fetchProjectLatestUpdateInternal(initiatingService, info.last_change,semantic, sessionPtr, ec, yield, mctx);
        if(ec) {
            return std::nullopt;
        }
        return info;
    }


    std::optional<WiActorView> PdmService::fetchProjectLatestUpdate(
            std::size_t initiatingService,
            WiFetchProjectLatestUpdateQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService,sessionPtr);
        WiRawActorInfoPart actor_part;
        WiActorView last_change;

        fetchProjectLatestUpdateInternal(initiatingService, last_change,query.semantic, sessionPtr, ec, yield, mctx);
        if(ec) {
            return std::nullopt;
        }

        return last_change;
    }

    void PdmService::fetchProjectLatestUpdateInternal(
            std::size_t initiatingService,
            WiActorView& _last_change,
            const std::string &semantic,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        WiRawActorInfoPart actor_part;

        std::vector<std::int64_t> roles = {
            PdmRoles::BlankComponent,
            PdmRoles::Container,
            PdmRoles::ElectricComponent,
            PdmRoles::Product,
            PdmRoles::ProxyComponent,
            PdmRoles::ProjectRbds,
            PdmRoles::RbdSchema,
            PdmRoles::RbdBlock,
            PdmRoles::Project
        };

        DataAccessConst().fetchLastUpdatedChildActor(actor_part,semantic,roles,0, mctx, ec,yield);
        if(ec){
            return;
        }
        auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
            return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };
        using FioResolverType = decltype(fioResolver);
        wi::basic_services::apply<FioResolverType>(
            actor_part,
            _last_change,
            ec,
            std::forward<FioResolverType>(fioResolver));
        if(ec) {
            return;
        }
    }

    std::optional<WiPaginatedView> PdmService::fetchProductRNView(
            std::size_t initiatingService,
            const WiFetchRNTableElementsQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        boost::ignore_unused(initiatingService, sessionPtr);
        return fetchProductRNViewInternal(initiatingService,query, sessionPtr,ec,yield,ctx);
    }

    std::optional<WiPaginatedView> PdmService::fetchProductRNViewInternal(
            std::size_t initiatingService,
            const WiFetchRNTableElementsQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        WiPaginatedView view;

        checkNode(initiatingService, query.semantic, PdmRoles::Product,sessionPtr,  ec, yield, mctx);
        if(ec) return std::nullopt;

        DataAccessConst().fetchPdmPaginatedViewCount(view.count, query.semantic, PdmRoles::ElementGroupWithProduct, 0, mctx, ec, yield);
        if(ec){
            return std::nullopt;
        }
        DataAccessConst().fetchPdmPaginatedView(view.nodes, query.semantic, PdmRoles::ElementGroupWithProduct, 0, query.range.offset, query.range.size, mctx, ec, yield);
        if(ec){
            return std::nullopt;
        }
        return view;
    }

    std::optional<WiFileResponse> PdmService::exportRNComponentsToXlsx(
            std::size_t initiatingService,
            const WiExportRNComponentsQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService, sessionPtr);
        WiPdmRawTreeNodeEntity::Container container;

        checkNode(initiatingService, query.semantic, PdmRoles::Product, sessionPtr,ec, yield, mctx);
        if (ec) return std::nullopt;

        DataAccessConst().fetchPdmRawTreeNodesEntity(container, query.semantic, mctx, ec, yield);
        if (ec) return std::nullopt;

        const auto exportSettings = fromJson<WiXlsxExportSettings>(query.settings);
        auto headers = exportSettings.headers;
        if (headers.empty() || !exportSettings.fileName.has_value()) {
            ec = make_error_code(export_settings_empty);
            return std::nullopt;
        }

        const auto groupsSettings = exportSettings.groupBy;
        std::vector<std::string> groupKeys;
        std::vector<std::string> groupNames;
        std::vector<GroupData> groupsData;
        for (const auto &groupData : groupsSettings) {
            if (groupData.key.has_value()) {
                groupKeys.push_back(groupData.key.value());

                std::string groupName;
                if (groupData.groupName.has_value()) groupName = groupData.groupName.value();
                    groupNames.push_back(groupName);

                groupsData.push_back(GroupData(groupData.key.value(), groupName));
            }
        }

        // Подготавливаем хэдеры, для корректной обработки пропусков столбцов
        std::sort(headers.begin(), headers.end(), [] (const WiXlsxExportHeader &first, const WiXlsxExportHeader &second) {return first.order < second.order;});
        const auto minColumn = headers.front().order;
        const auto maxColumn = headers.back().order;

        if (minColumn > 1) {
            for (int column = minColumn; column != 1; --column) {
                WiXlsxExportHeader emptyHeader;
                emptyHeader.order = column - 1;

                headers.insert(headers.begin(), emptyHeader);
            }
        }

        std::vector<WiXlsxExportHeader> preparedHeaders;
        for (int column = 1; column < maxColumn + 1; ++column) {
            WiXlsxExportHeader header;
            header.order = column;
            if (auto iter = std::find_if(headers.begin(), headers.end(), [column] (const WiXlsxExportHeader &item) {return item.order == column;}); iter != headers.end())
                header = *iter;

            preparedHeaders.push_back(header);
        }

        const auto getTranslatedValue = [] (WiTranslateText::Container container, const int code = 1049) {
            std::string value;
            auto iter = std::find_if(container.begin(), container.end(), [code] (const std::pair<const int, std::string> &item) {return item.first == code;});
            if (iter != container.end())
                value = iter->second;

            return value;
        };

        // Собираем данные по справочникам
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> componentClasses;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> componentTypes;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> componentTypeFormulas;
        std::unordered_map<std::string, std::unordered_map<std::string, std::set<std::string>>> componentTypeParameters;
        std::unordered_map<std::string, std::string> handbookNames;
        std::unordered_map<std::string, reliability::handbooks::HandbookParameterTranslates> handbooksParameterTranslate;

        const std::optional<std::vector<WiHandbookMetadata>> handbooks = getHandbooksMetadata(initiatingService, sessionPtr, ec, yield, ctx);
        if (handbooks.has_value()) {
            for (const auto &handbook : handbooks.value()) {
                handbooksParameterTranslate[handbook.code] = handbook.parameter_translates;
                handbookNames[handbook.code] = getTranslatedValue(handbook.name, 1033); // NOTE почему-то на русском значение лежит под 1033, хотя в других местах под 1049
                for (const auto &[key, classMetaData] : handbook.items) {
                    for (const auto &[type, typeMetaData] : classMetaData.types) {
                        componentClasses[handbook.code][type] = getTranslatedValue(classMetaData.name);
                        componentTypes[handbook.code][type] = getTranslatedValue(typeMetaData.name);

                        // некоторые формулы в справочниках могут отличаться от того, что нужно при выводе в excel
                        auto formula = typeMetaData.meta.formula;
                        if (ExportVariables::MagneticBubbleStorageDevicesFormula == formula)
                            formula = ExportVariables::MagneticBubbleStorageDevicesFormulaToShow;
                        else if (ExportVariables::ConnectionAssemblyFormula == formula)
                            formula = ExportVariables::ConnectionAssemblyFormulaToShow;
                        else if (ExportVariables::Lambda_p == formula)
                            formula = ExportVariables::Lambda;

                        componentTypeFormulas[handbook.code][type] = formula;

                        // удаляем ненужные параметры
                        auto outParamsInRightOrder = typeMetaData.meta.out_parameters;
                        std::set<std::string> outParams(outParamsInRightOrder.begin(),
                                                        outParamsInRightOrder.end());
                        outParams.erase(ExportVariables::Failure_rate);
                        if (ExportVariables::SurfaceMountConnectorAssembly == type)
                            outParams.erase(ExportVariables::L_SMT);

                        componentTypeParameters[handbook.code][type] = outParams;
                    }
                }
            }
        }

        auto elementRoles = PdmRoles::ElementGroupWithProduct;
        std::set<std::int32_t> roles(elementRoles.begin(), elementRoles.end());
        roles.erase(PdmRoles::Product);

        // Формирование данных компонентов
        std::unordered_map<std::int64_t, std::shared_ptr<Component>> componentsData;
        for (const auto& rawTreeNodeEntity : container) {
            const auto it = roles.find(rawTreeNodeEntity.role);
            if (it == roles.end()) continue;

            if (!rawTreeNodeEntity.entity.has_value()) continue;
            const auto entity = rawTreeNodeEntity.entity;

            if (!entity->data.has_value() || !rawTreeNodeEntity.extension.has_value()) continue;
            const auto data = fromJson<WiPdmComponentData>(entity->data);
            const auto ex = fromJson<WiPdmElementExtension>(rawTreeNodeEntity.extension);

            std::vector<Cell> tableInfo;
            std::unordered_map<std::string, Cell> componentData;
            if (PdmRoles::ElectricComponent == rawTreeNodeEntity.role) {
                const auto handbook = data.handbook;
                if (handbook.has_value()) {
                    const auto handbookCode = handbook->code;
                    const auto handbookName = handbookNames[handbookCode];
                    const auto componentClass = componentClasses[handbookCode][data.type.value()];
                    auto componentType = componentTypes[handbookCode][data.type.value()];

                    // Костыль для правильного наименования групп типов компонентов
                    if (ExportVariables::milPartStressName == handbookName) {
                        const auto groupType = exportSortOrder::mil217f_PS_typeGroups;
                        auto iter = std::find_if(groupType.begin(), groupType.end(), [componentType] (const std::pair<std::string, std::set<std::string>> &item) {return item.second.find(componentType) != item.second.end();});
                        if (iter != groupType.end())
                            componentType = iter->first;

                        if ("Лампа постоянного тока" == componentType || componentType == "Лампа переменного тока")
                            componentType = "Лампа";
                    }
                    else if (ExportVariables::milPartCountName == handbookName) {
                        const auto groupType = exportSortOrder::mil217f_PC_typeGroups;
                        auto iter = std::find_if(groupType.begin(), groupType.end(), [componentType] (const std::pair<std::string, std::set<std::string>> &item) {return item.second.find(componentType) != item.second.end();});
                        if (iter != groupType.end())
                            componentType = iter->first;
                    }

                    componentData[ExportVariables::Handbook] = handbookName;
                    componentData[ExportVariables::ComponentClass] = componentClass;
                    componentData[ExportVariables::ComponentType] = componentType;
                    componentData[ExportVariables::Formula] = componentTypeFormulas[handbookCode][data.type.value()];
                }
            }

            componentData[ExportVariables::Position] = ex.position;
            componentData[ExportVariables::Name] = getTranslatedValue(rawTreeNodeEntity.header);
            componentData[ExportVariables::Description] = getTranslatedValue(rawTreeNodeEntity.description);
            componentData[ExportVariables::Letter_tag] = ex.letter_tag;
            componentData[ExportVariables::Designation] = ex.designation;
            componentData[ExportVariables::ProductType] = rawTreeNodeEntity.role;

            const auto convertNumberToDouble = [] (const Number &value) {
                return value.convert_to<double>();
            };

            const auto setDoubleValue = [&componentData] (const std::string& key, const std::optional<Number> &number, const int multiplier = 1) {
                if (!number.has_value()) {
                    componentData[key] = "";
                    return;
                }

                auto doubleValue = number.value().convert_to<double>();
                if (multiplier != 1)
                    doubleValue = doubleValue * multiplier;

                componentData[key] = doubleValue;
            };

            setDoubleValue(ExportVariables::MTBF, data.variables->MTBF);
            setDoubleValue(ExportVariables::Failure_rate, data.variables->failure_rate, 1000000);
            setDoubleValue(ExportVariables::Reliability, data.variables->reliability);
            setDoubleValue(ExportVariables::Failure_probability, data.variables->failure_probability);

            Row componentRow;
            Row tableHeaders;
            for (const auto &header : preparedHeaders) {
                if (!header.key.has_value() || !header.header.has_value()) {
                    componentRow.emplace_back("");
                    tableHeaders.emplace_back("");
                    continue;
                }

                const auto key = header.key.value();
                const auto headerName = header.header.value();

                if (key != ExportVariables::Coefficients) {
                    componentRow.emplace_back(componentData[key]);
                    tableHeaders.emplace_back(headerName);
                }
                else {
                    reliability::handbooks::ParameterMap componentParameters;
                    if (data.parameters.has_value())
                        componentParameters = data.parameters.value();

                    if (!data.handbook.has_value() || !data.type.has_value())
                        continue;

                    for (const auto &handbookParam : componentTypeParameters[data.handbook.value().code][data.type.value()]) {
                        auto paramName = handbookParam;

                        auto handbookTranslatesIt = handbooksParameterTranslate.find(data.handbook.value().code);
                        if (handbookTranslatesIt != handbooksParameterTranslate.end()) {
                            auto translatesIt = handbookTranslatesIt->second.find(paramName);
                            if (translatesIt != handbookTranslatesIt->second.end()) {
                                paramName = getTranslatedValue(translatesIt->second.name);
                            }
                        }

                        tableHeaders.emplace_back(paramName);
                        const auto iter = componentParameters.find(handbookParam);
                        if (componentParameters.end() == iter) {
                            componentRow.emplace_back("");
                            continue;
                        }

                        if (!iter->second.value.has_value()) {
                            componentRow.emplace_back("");
                            continue;
                        }

                        const auto parameterValue = iter->second.value.value();
                        std::visit([&componentRow, &convertNumberToDouble](auto&& arg)
                        {
                            using ValueType = typename std::decay<decltype(arg)>::type;
                            if constexpr(std::is_same<ValueType, std::optional<std::string>>::value) {
                                const auto val = static_cast<std::optional<std::string>>(arg);
                                if (val.has_value()) componentRow.emplace_back(val.value());
                                else componentRow.emplace_back("");
                            }
                            else if constexpr(std::is_same<ValueType, std::optional<Number>>::value) {
                                const auto val = static_cast<std::optional<Number>>(arg);
                                if (val.has_value()) componentRow.emplace_back(convertNumberToDouble(val.value()));
                                else componentRow.emplace_back("");
                            }
                            else if constexpr(std::is_same<ValueType, std::optional<long int>>::value) {
                                const auto val = static_cast<std::optional<long int>>(arg);
                                if (val.has_value()) componentRow.emplace_back(convertNumberToDouble(val.value()));
                                else componentRow.emplace_back("");
                            }
                            else if constexpr(std::is_same<ValueType, long int>::value) {
                            }
                            else {
                                componentRow.emplace_back("");
                            }
                        }, parameterValue);
                    }
                }
            }

            ComponentType type;
            if (rawTreeNodeEntity.role == PdmRoles::Container) type = ComponentType::Container;
            else if (rawTreeNodeEntity.role == PdmRoles::ProxyComponent) type = ComponentType::ProxyComponent;
            else if (rawTreeNodeEntity.role == PdmRoles::ElectricComponent) type = ComponentType::ElectricComponent;

            componentsData[rawTreeNodeEntity.id] = std::make_shared<Component>(rawTreeNodeEntity.depth, rawTreeNodeEntity.ancestor, type, componentData, tableHeaders, componentRow);
        }

        const auto fileUuidName = XlsxWriter::getFileUuidName();
        ComponentGrouper componentGrouper(componentsData, groupsData);
        XlntXlsxWriter writer;
        writer.writeXlsx(componentGrouper.createGroupedTables(), (XlsxWriter::getSavePath() / fileUuidName).string());

        const auto relativePath = boost::filesystem::path(H_TO_S(StringHelper::TempDirectory)) / fileUuidName;
        std::optional response = WiFileResponse();
        response.value().setFileData(WiFileResponse::excel, relativePath.string(), exportSettings.fileName.value());
        return response;
    }

    std::optional<WiPdmTreeView> PdmService::fetchProjectView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        auto session =std::reinterpret_pointer_cast<Session>(sessionPtr->session());
        session->addProject(semantic);

        return fetchTree<WiPdmTreeItemView>(initiatingService, semantic, language, PdmRoles::Project, sessionPtr, ec, yield, mctx);
    }

    std::optional<WiRawTree<WiPdmTreeEntityItemView>> PdmService::fetchElementStructure(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        return fetchTree<WiPdmTreeEntityItemView>(initiatingService, semantic, language, sessionPtr, ec, yield,mctx);
    }

    std::optional<WiPdmRawEntity::Container> PdmService::fetchElementsData(
            std::size_t initiatingService,
            const WiSemanticsOnlyQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const boost::asio::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        WiPdmRawEntity::Container container;
        DataAccessConst().fetchPdmElementsData(container, query.semantics, mctx, ec, yield);
        if (ec) return
            std::nullopt;

        return container;
    }

    std::optional<WiRawTree<WiPdmTreeEntityItemView>> PdmService::fetchElementStructureUsingDepth(
        std::size_t initiatingService,
        const WiSemanticAndDepthQuery &query,
        const std::shared_ptr<IWiSession> &sessionPtr,
        boost::system::error_code &ec,
        const boost::asio::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        WiPdmRawTreeNodeEntity::Container container;
        DataAccessConst().fetchPdmEntityTreeUsingDepth(container, query.semantic, query.depth, mctx, ec, yield);
        if (ec)
            return std::nullopt;

        auto statusResolver = [this](auto &&id, auto &&ec) {
            return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
            return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        using StatusResolverType = decltype(statusResolver);
        using FioResolverType = decltype(fioResolver);

        WiPdmTreeNodeEntityView::Container res;
        wi::basic_services::apply<WiPdmRawTreeNodeEntity, WiPdmTreeNodeEntityView, WiPdmStatus, StatusResolverType, FioResolverType>(
            container,
            res,
            m_defaultLanguage,
            ec,
            std::forward<StatusResolverType>(statusResolver),
            std::forward<FioResolverType>(fioResolver));

        WiRawTree<WiPdmTreeEntityItemView> treeView(res);
        if (!treeView.has_data()) {
            ec = make_error_code(node_not_found);
            return std::nullopt;
        }

        return std::make_optional<WiRawTree<WiPdmTreeEntityItemView>>(std::move(treeView));
    }

    void PdmService::closeProjectView(
            std::size_t initiatingService,
            const WiSemanticOnlyQuery &query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        auto session = std::reinterpret_pointer_cast<Session>(sessionPtr->session());
        session->removeProject(query.semantic);
    }

    std::optional<WiPdmNodeView> PdmService::fetchNodeView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        boost::ignore_unused(sessionPtr);
        boost::ignore_unused(initiatingService);

        auto rawNode = fetchRawNode(initiatingService,semantic,sessionPtr, ec, yield, mctx);
        if (ec) {
            return std::nullopt;
        }
        if(!rawNode) {
            ec = make_error_code(node_not_found);
        }

        auto statusResolver = [this](auto &&id, auto &&ec) {
            return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
            return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        using StatusResolverType = decltype(statusResolver);
        using FioResolverType = decltype(fioResolver);

        WiPdmNodeView nodeView;
        wi::basic_services::apply<WiPdmStatus, StatusResolverType, FioResolverType>(
                *rawNode,
                nodeView,
                language,
                ec,
                std::forward<StatusResolverType>(statusResolver),
                std::forward<FioResolverType>(fioResolver));
        if (ec) {
            return std::nullopt;
        }

        return std::make_optional<WiPdmNodeView>(std::move(nodeView));
    }

    std::optional<WiPdmNodeView::Container> PdmService::fetchNodesView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        WiPdmNodeView::Container container;
        fetchNodesView<WiPdmNodeView>(initiatingService, container, semantic, language, sessionPtr, ec, yield, mctx);

        if (ec) {
            return std::nullopt;
        }

        return std::make_optional<WiPdmNodeView::Container>(std::move(container));
    }

    std::optional<WiPdmNodeView::Container> PdmService::fetchProjectComponents(
        std::size_t initiatingService,
        const std::string &semantic,
        std::optional<std::int32_t> &language,
        const std::shared_ptr<IWiSession> &sessionPtr,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        checkNode(initiatingService, semantic, PdmRoles::Project, sessionPtr,  ec, yield, mctx);
        if(ec) return std::nullopt;
        auto settings = nodeNearestDescendant(initiatingService, semantic, PdmRoles::ProjectSettingsComponents, 1, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        return fetchNodesView(initiatingService, settings.semantic, language, sessionPtr, ec, yield, mctx);
    }

    std::optional<WiPdmEntityView> PdmService::fetchEntityView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        boost::ignore_unused(sessionPtr);
        boost::ignore_unused(initiatingService);

        WiPdmRawEntity rawEntity;
        fetchRawEntity(initiatingService,semantic, rawEntity,sessionPtr, ec, yield, mctx);
        if (ec) {
            return std::nullopt;
        }

        auto statusResolver = [this](auto &&id, auto &&ec) {
            return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
            return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        using StatusResolverType = decltype(statusResolver);
        using FioResolverType = decltype(fioResolver);

        WiPdmEntityView entityView;
        wi::basic_services::apply<WiPdmStatus, StatusResolverType, FioResolverType>(
                rawEntity,
                entityView,
                language,
                ec,
                std::forward<StatusResolverType>(statusResolver),
                std::forward<FioResolverType>(fioResolver));
        if (ec) {
            return std::nullopt;
        }
        return std::make_optional<WiPdmEntityView>(std::move(entityView));
    }

    std::optional<WiPdmTreeView> PdmService::fetchTreeRoleView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            std::int32_t role,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        return fetchTree<WiPdmTreeItemView>(initiatingService, semantic, language, role, sessionPtr, ec, yield, mctx);
    }

    std::optional<WiPdmTreeEntityView> PdmService::fetchTreeEntityRoleView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            std::int32_t role,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        return fetchTree<WiPdmTreeEntityItemView>(initiatingService, semantic, language, role, sessionPtr, ec, yield, mctx);
    }

    std::optional<WiPdmTreeEntityView> PdmService::fetchTreeCompoundEntityView(
            std::size_t initiatingService,
            const std::string &semantic,
            std::optional<std::int32_t> &language,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        return fetchTreeCompound<WiPdmTreeEntityItemView>(initiatingService, semantic, language, sessionPtr, ec, yield, mctx);
    }

    /*
    * end public section
    */

    std::optional<WiPdmStatus> PdmService::getStatus(std::int64_t id, boost::system::error_code &ec) const noexcept(true) {
        auto const findIter = m_statuses.find(id);
        if (findIter == m_statuses.cend()) {
            ec = make_error_code(pdm_status_not_found);
            WI_LOG_ERROR() <<  __FILE__ << " " << __PRETTY_FUNCTION__ << " " << ec.what();
            return std::nullopt;
        } else {
            return std::make_optional<WiPdmStatus>(findIter->second);
        }
    }

//    template<typename Result, typename BinaryOperation>
//    Result PdmService::accumulateOverShortNodes(std::string &semantic, const std::shared_ptr<IWiSession>& sessionPtr, boost::system::error_code &ec, const net::yield_context &yield, Result init, BinaryOperation op){
//        WiSemanticQuery query{semantic, WI_CONFIGURATION().read_settings<std::int32_t>(server_default_language)};
//        std::shared_ptr<WiCacheWrapper<WiPdmShortNode::Container>> response = fetchPdmShortNodes(query, sessionPtr, ec, yield);
//        auto nodes = fromJson<WiPdmShortNode::Container>( (*response).storage() );
//        return std::move(std::accumulate(nodes.begin(), nodes.end(), init, op));
//    }

    inline std::shared_ptr<WiPdmRawNode> PdmService::fetchRawNode(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx,bool forceupdate) const noexcept(true) {
        GUARD_PDM_METHOD();
        boost::ignore_unused(forceupdate);
        return WiCacheSvc.getAsync<WiPdmRawNode>(semantic, mctx, ec, yield);
    }

    inline std::shared_ptr<WiPdmRawNodeEntity> PdmService::fetchRawNodeEntity(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        return WiCacheSvc.getAsync<WiPdmRawNodeEntity>(semantic, mctx, ec, yield);
    }

    inline void PdmService::fetchRawNodes(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawNode::Container &container,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        DataAccessConst().fetchPdmRawNodes(container, semantic, mctx, ec, yield);
    }

    inline void PdmService::fetchRawNodesEntity(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawNodeEntity::Container &container,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        DataAccessConst().fetchPdmRawNodesEntity(container, semantic, mctx, ec, yield);
    }

    inline void PdmService::fetchFlatRawTree(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawTreeNode::Container &container,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        boost::ignore_unused(sessionPtr);
        boost::ignore_unused(initiatingService);
        DataAccessConst().fetchPdmRawTreeNodes(container, semantic, mctx, ec, yield);
    }

    inline void PdmService::fetchFlatRawTree(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawTreeNodeEntity::Container &container,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        boost::ignore_unused(sessionPtr);
        boost::ignore_unused(initiatingService);
        DataAccessConst().fetchPdmRawTreeNodesEntity(container, semantic, mctx, ec, yield);
    }

    void PdmService::fetchRawEntity(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawEntity &entity,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        WiPdmRawEntity::Container container;
        fetchReverseEntityTree(initiatingService,semantic, container,sessionPtr, ec, yield, mctx);
        if (ec) {
            return;
        }
        // todo сделать объединение данных узла
        if (container.empty()) {
            ec = make_error_code(node_entity_not_found);
            return;
        }
        entity = std::move(std::move(container.front()));
    }

    void PdmService::fetchReverseEntityTree(
            std::size_t initiatingService,
            const std::string &semantic,
            WiPdmRawEntity::Container &container,
            const std::shared_ptr<IWiSession>& sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        auto node = WiCacheSvc.getAsync<WiPdmRawNode>(semantic, mctx, ec, yield);
        if (!node) {
            WI_LOG_ERROR() <<  __FILE__ << " " << __PRETTY_FUNCTION__ << " " << ec.what();
            ec = make_error_code(node_not_found);
            return;
        }

        WiPdmRawEntityInternal::Container rawEntityInternalContainer;
        DataAccessConst().fetchPdmReverseEntityTree(rawEntityInternalContainer, node->id, mctx, ec, yield);

        if (ec) {
            WI_LOG_ERROR() <<  __FILE__ << " " << __PRETTY_FUNCTION__ << " " << ec.what();
            ec = make_error_code(internal_error);
            return;
        }

        if (rawEntityInternalContainer.empty()) {
            ec = make_error_code(node_not_found);
            return;
        }

        boost::for_each(rawEntityInternalContainer, [&](auto &item) {
            if (item.entity.has_value()) {
                container.push_back(std::move(item.entity.value()));
            }
        });
    }

    std::string PdmService::addNewNodeInternal(
            std::size_t initiatingService,
            WiNewPdmNodeQuery &query,
            const std::shared_ptr<IWiSession> sessionPtr,
            const Filter &filter,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        boost::ignore_unused(initiatingService);

        constexpr static const char *semanticSplitter = "::";
        std::int64_t nodeId = 0;
        std::string semantic = query.parent;
        std::string random = std::to_string((*m_mt)());

        semantic.append(semanticSplitter);
        semantic.append(random);

        std::string headerSemantic = semantic;
        headerSemantic.append(semanticSplitter);
        headerSemantic.append("header");

        std::string descriptionSemantic = semantic;
        descriptionSemantic.append(semanticSplitter);
        descriptionSemantic.append("description");

        auto entityStatus = std::make_optional<std::int64_t>(1);
        auto entityOldStatusData = std::optional<nlohmann::json>{};
        auto entityStatusData = std::optional<nlohmann::json>{};
        auto entityV = std::optional<std::int32_t>{1};
        auto nodeV = std::optional<std::int32_t>{1};
        auto nodeStatus = std::optional<std::int64_t>{1};
        auto nodeStatusData = std::optional<nlohmann::json>{};
        auto lockRole = std::optional<std::int32_t>{};
        auto lockVersion = std::optional<std::int32_t>{};
        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});

        auto parent = query.parent;
        auto selfSemantic = semantic;

        DataAccessConst().addPdmNode(
                nodeId, // id нового узла
                query.parent, // semantic родительского узла
                query.role, // role создаваемого узла
                semantic, // semantic нового узла
                headerSemantic, // semantic заголовка
                query.header, // заголовок
                descriptionSemantic, // semantic описания
                query.description, // описание
                query.data, // данные
                0, // old entity status
                entityStatus, // entityStatus,
                entityOldStatusData, //std::optional<nlohmann::json> &entityOldStatusData
                entityStatusData, //std::optional<nlohmann::json> &entityStatusData
                entityV, //entityV версия данных
                nodeV, //nodeV версия узла
                query.extension,
                query.type,
                actor, // актор
                nodeStatus, // статус
                nodeStatusData, // данные статуса
                lockRole, // role блокировки
                lockVersion, // версия блокировки
                mctx,
                ec,
                yield);

        if (!ec) {
            auto rawNewNodePtr = fetchRawNode(selfSemantic, ec, yield, mctx);
            if (!ec && rawNewNodePtr) {
                IWiPlatform::PdmAddNodeEvent event(m_eventNumerator);
                event.time_point = std::chrono::system_clock::now();
                event.initiator = initiatingService;
                event.userid = sessionPtr->userId();
//                event.fio = sessionPtr->getUserView();
                event.parent = parent;
                event.semantic = selfSemantic;
                event.filter = filter;
                event.newNode = std::make_optional<WiPdmRawNode>(*rawNewNodePtr);
                mctx.fire(std::forward<IWiPlatform::PdmAddNodeEvent>(event));
//                EventBusServiceSvc.fireAsync(std::forward<IWiPlatform::PdmAddNodeEvent>(event));
//                initiateRecalculation(initiatingService,sessionPtr,rawNewNodePtr->semantic,ec,yield,mctx);
//                if(ec) return selfSemantic;
            }
        }

        return selfSemantic;
    }

    std::optional<WiSemanticResult> PdmService::moveNodeInternal(
        std::size_t initiatingService,
        const WiMovePdmNodeQuery &query,
        const std::shared_ptr<IWiSession> sessionPtr,
        const Filter &filter,
        boost::system::error_code &ec,
        const net::yield_context &yield,
        std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();
        WiSemanticResult res;
        res.semantic = query.semantic;
        auto actor = std::make_optional<WiRawActorInfoPart> ({sessionPtr->userId(), std::chrono::system_clock::now()});
        if(ec) return std::nullopt;

        auto rawOldNodePtr = fetchRawNode(res.semantic, ec, yield, mctx);
        WiPdmRawTreeNode::Container descendants;
        fetchFlatRawTree(initiatingService,res.semantic,descendants, sessionPtr, ec, yield, mctx);

        std::map<std::string, std::vector<std::string>> rbd_refs_descendants;
        for(const auto &node : descendants){
            //unbind descendants from RBD and store refs
            std::optional<std::vector<std::string>> rbd_refs_;
            if(node.role == PdmRoles::ElectricComponent
               || node.role == PdmRoles::ProxyComponent
               || node.role == PdmRoles::Container)
            {
                rbd_refs_ = removeRbdBindingFromElement(initiatingService,node.semantic,sessionPtr,ec,yield,mctx);
                if(ec) return std::nullopt;
            }
            if(rbd_refs_.has_value())
            {
                rbd_refs_descendants.insert({node.semantic, rbd_refs_.value()});
            }

            initiateRecalculation(initiatingService,sessionPtr,node.semantic,ec,yield,mctx);
            if(ec) return std::nullopt;
        }

        if (rawOldNodePtr) {
            DataAccessConst().movePdmNode(0, 0, res.semantic, query.destination, actor, mctx, ec, yield);
            if(!ec) {
                updateElementSemanticInFailureTypeCache(initiatingService, rawOldNodePtr->semantic, res.semantic, sessionPtr, ec, yield, mctx);

                WiCacheSvc.clear(rawOldNodePtr->semantic);
                // force update updated node
                auto newNodePtr = fetchRawNode(res.semantic, ec, yield, mctx, true);
                if (!ec && newNodePtr) {
                    std::optional<std::string> parentOpt = std::nullopt;
                    {
                        constexpr static const char *semanticSplitter = "::";
                        std::vector<std::string> semantics;
                        boost::iter_split(semantics, res.semantic, boost::first_finder(semanticSplitter));

                        if (!semantics.empty()) {
                            semantics.erase(semantics.end() - 1);
                            std::string parent;
                            if (!semantics.empty()) {
                                buildSemantic(semantics, parent);
                                parentOpt = std::make_optional<std::string>(std::move(parent));
                            }
                        }
                    }

                    IWiPlatform::PdmUpdateNodeEvent event(m_eventNumerator);
                    // prohibit cache clearing onEvent
                    // since cahce already contains current version of the node
                    event.clearCached = false;
                    event.time_point = std::chrono::system_clock::now();
                    event.initiator = initiatingService;
                    event.userid = sessionPtr->userId();
//                    event.fio = sessionPtr->getUserView();
                    event.parent = parentOpt;
                    event.semantic = res.semantic;
                    event.filter = filter;
                    event.newNode = std::make_optional<WiPdmRawNode>(*newNodePtr);
                    event.oldNode = std::make_optional<WiPdmRawNode>(*rawOldNodePtr);
                    mctx.fire(std::forward<IWiPlatform::PdmUpdateNodeEvent>(event));
                    initiateRecalculation(initiatingService,sessionPtr,newNodePtr->semantic,ec,yield,mctx);
                    if(ec) return std::nullopt;

                    //rebind rbd refs
                    if(rbd_refs_descendants.count(descendants.front().semantic))
                    {
                        for(const auto& rbd_semantic: rbd_refs_descendants[descendants.front().semantic])
                        {
                            BindPdmComponentWithRbdBlockInternal(
                                initiatingService,
                                newNodePtr->semantic,
                                rbd_semantic,
                                sessionPtr,
                                ec,
                                yield,
                                mctx);
                            if(ec) return std::nullopt;
                        }
                    }

                    // fire events for all child nodes
                    for(auto it = std::next(descendants.begin()); it < descendants.end();++it){
                        IWiPlatform::PdmUpdateNodeEvent child_event(m_eventNumerator);
                        auto oldNode = *it;
                        WiCacheSvc.clear(oldNode.semantic);

                        child_event.clearCached = false;
                        child_event.time_point = std::chrono::system_clock::now();
                        child_event.initiator = initiatingService;
                        child_event.userid = sessionPtr->userId();
//                        child_event.fio = sessionPtr->getUserView();
                        child_event.userid = event.userid;
//                        child_event.fio = event.fio;

                        child_event.oldNode = std::make_optional<WiPdmRawNode>(oldNode);
                        if(oldNode.semantic.compare(0,rawOldNodePtr->semantic.size(),rawOldNodePtr->semantic)){
                            ec = make_error_code(error::internal_error);
                        } else{
                            // ok
                            child_event.semantic = oldNode.semantic = newNodePtr->semantic + oldNode.semantic.substr(rawOldNodePtr->semantic.size());
                        }
                        {
                            constexpr static const char *semanticSplitter = "::";
                            std::vector<std::string> semantics;
                            boost::iter_split(semantics, event.semantic, boost::first_finder(semanticSplitter));

                            if (!semantics.empty()) {
                                semantics.erase(semantics.end() - 1);
                                std::string parent;
                                if (!semantics.empty()) {
                                    buildSemantic(semantics, parent);
                                    parentOpt = std::make_optional<std::string>(std::move(parent));
                                }
                            }
                        }

                        //rebind rbd refs
                        std::string oldNode_semantic = it->semantic;
                        if(rbd_refs_descendants.count(oldNode_semantic))
                        {
                            for(const auto& rbd_semantic: rbd_refs_descendants[oldNode_semantic])
                            {
                                BindPdmComponentWithRbdBlockInternal(
                                    initiatingService,
                                    oldNode.semantic,
                                    rbd_semantic,
                                    sessionPtr,
                                    ec,
                                    yield,
                                    mctx);
                                if(ec) return std::nullopt;
                            }
                        }

                        child_event.filter = filter;
                        child_event.parent = parentOpt;
                        child_event.newNode = std::make_optional<WiPdmRawNode>(oldNode);
                        mctx.fire(std::forward<IWiPlatform::PdmUpdateNodeEvent>(child_event));
                        initiateRecalculation(initiatingService,sessionPtr,oldNode.semantic,ec,yield,mctx);
                        if(ec) return std::nullopt;
                    }
                    if(ec) return std::nullopt;
                }
            }
        }
        else {
            ec = make_error_code(code::node_not_found);
        }
        return res;
    }

    inline void PdmService::checkNode(
            std::size_t initiatingService,
            const std::string &semantic,
            std::int32_t role,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true) {
        GUARD_PDM_METHOD();

        const auto node = fetchRawNode(initiatingService,semantic,sessionPtr, ec, yield, mctx);
        if (ec) {
            return;
        }
        if (!node) {
            ec = make_error_code(node_not_found);
        }

        if(node->role != role) {
            ec = make_error_code(invalid_node_role);
        }
    }

    void PdmService::rearrangePositionalIndecies(
            std::size_t initiatingService,
            const WiRearrangePostionalIndecies& query,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend_mode){
            destination = parentSemantic(query.destination, ec);
            if(ec) return;
        }
        auto node = fetchRawNode(destination, ec, yield, mctx);
        if(ec || !node) return;

        std::vector<std::uint32_t> pos;
        if(node->extension.has_value() &&
        (  node->role == PdmRoles::BlankComponent
        || node->role == PdmRoles::ProxyComponent
        || node->role == PdmRoles::ElectricComponent
        || node->role == PdmRoles::Container)){
            std::string position = fromJson<WiPdmElementExtension>(node->extension.value()).position;
            if(!unwrapPositional(pos, position)){
                ec = make_error_code(error::cant_assign_positional_index);
                return;
            }
        }else if(node->role != PdmRoles::Product){
            ec = make_error_code(error::cant_assign_positional_index);
            return;
        }

        WiPdmRawNode::Container nodes;
        fetchRawNodes(initiatingService,destination, nodes, sessionPtr, ec, yield, mctx);
        if(ec ) return;
        if(nodes.size() == 0) return;

        auto default_comparer = DefaultPositionalComparer(pos);
        if(query.prepend_mode) {
            // find element to which we will need to prepend
            auto it = std::find_if(nodes.begin(), nodes.end(), [&query](const auto&item){
                return item.semantic == query.destination;
            });
            if(it >= nodes.end()){
                ec = make_error_code(error::node_not_found);
                return;
            }
            auto reference_node = *it;
            std::sort(nodes.begin(), nodes.end(), [&query, &reference_node, &default_comparer](const auto&left, const auto&right){
                bool l_req_p = false, r_req_p = false;
                l_req_p = left.role == PdmRoles::Container || left.role == PdmRoles::ProxyComponent || left.role == PdmRoles::ElectricComponent;
                r_req_p = right.role == PdmRoles::Container || right.role == PdmRoles::ProxyComponent || right.role == PdmRoles::ElectricComponent;
                if(!l_req_p && !r_req_p)
                    return left.id<right.id;
                else if(!l_req_p || !r_req_p)
                    return l_req_p > r_req_p;

                auto il = std::find_if(query.elements.begin(), query.elements.end(), [&left](const auto&item){
                    return left.semantic == item;
                });
                auto ir = std::find_if(query.elements.begin(), query.elements.end(), [&right](const auto&item){
                    return right.semantic == item;
                });
                bool l_is_el = il < query.elements.end(), r_is_el = ir < query.elements.end();

                if((l_is_el) || (r_is_el)){ // if one of them is in elements array
                    if(l_is_el && r_is_el){ // if both of them in array
                        return il < ir;  // return the one that appears in array first
                    }else{ // otherwise only one of them is present in array
                        if(l_is_el){ // if left one need rearrangement
                            if(right.semantic == reference_node.semantic){
                                return true;
                            } else{
                                return default_comparer(reference_node, right);
                            }
                        } else { // the right one needs rearrangement
                            if(left.semantic == reference_node.semantic){
                                return false;
                            }else {
                                return default_comparer(left, reference_node);
                            }
                        }
                    }
                }else{
                    // otherwise just compare them
                    return default_comparer(left, right);
                }
            });
        }else{
            std::sort(nodes.begin(), nodes.end(), [&query, &default_comparer](const auto&left, const auto&right){
                bool l_req_p = false, r_req_p = false;
                l_req_p = left.role == PdmRoles::Container || left.role == PdmRoles::ProxyComponent || left.role == PdmRoles::ElectricComponent;
                r_req_p = right.role == PdmRoles::Container || right.role == PdmRoles::ProxyComponent || right.role == PdmRoles::ElectricComponent;
                if(!l_req_p && !r_req_p)
                    return left.id<right.id;
                else if(!l_req_p || !r_req_p)
                    return l_req_p > r_req_p;

                auto il = std::find_if(query.elements.begin(), query.elements.end(), [&left](const auto&item){
                    return left.semantic == item;
                });
                auto ir = std::find_if(query.elements.begin(), query.elements.end(), [&right](const auto&item){
                    return right.semantic == item;
                });
                bool l_is_el = il < query.elements.end(), r_is_el = ir < query.elements.end();

                if((l_is_el) || (r_is_el)){ // if one of them is in elements array
                    if(l_is_el && r_is_el){ // if both of them in array
                        return il < ir;  // return the one that appears in array first
                    }else{ // otherwise only one of them is present in array
                        return l_is_el < r_is_el; // return true if right element appears, and left doesnt
                    }
                }else{
                    // otherwise both of them should be nodes that dont need to be rearranged
                    return default_comparer(left, right);
                }
            });
        }

        pos.push_back(1);
        assignPositionals(initiatingService, pos, nodes, sessionPtr, ec, yield, 1, mctx);
    }

    void PdmService::assignPositionals(
            std::size_t initiatingService,
            std::vector<std::uint32_t> &pos,
            const WiPdmRawNode::Container& nodes,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::size_t depth,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(pos.size() == 0) {
            ec = make_error_code(error::cant_assign_positional_index);
            return;
        }
        for(const auto&node : nodes){
            if(node.role != PdmRoles::Container && node.role != PdmRoles::ProxyComponent && node.role != PdmRoles::ElectricComponent)
                return;
            std::string sem = node.semantic;
            assignPositional(initiatingService, pos, sem, sessionPtr, ec, yield, depth, mctx);
            if(ec){
                return;
            }
            auto it = pos.rbegin();
            if(it >= pos.rend()){
                ec = make_error_code(error::cant_assign_positional_index);
                return;
            }
            (*it)++;
        }
    }

    void PdmService::assignPositional(
            std::size_t initiatingService,
            std::vector<std::uint32_t> &pos,
            std::string& semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::size_t depth,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        std::vector<std::uint32_t> oldPos;
        if(pos.size() == 0) {
            ec = make_error_code(error::cant_assign_positional_index);
            return;
        }
        auto node = fetchRawNode(semantic, ec, yield, mctx);
        if(ec || !node) return;
        {
            WiPdmElementExtension ext;
            if(node->extension.has_value()){
                ext = fromJson<WiPdmElementExtension>(node->extension.value());
            }
            unwrapPositional(oldPos, ext.position);
            std::string position, oldPosition = ext.position;
            if(DefaultPositionalComparer::lexcompare(oldPos, pos) != 0) {// if old and new positional are not identical
                boost::system::error_code tec;
                buildPositional(pos, position, tec);
                if(tec){
                    ec = make_error_code(error::cant_assign_positional_index);
                    return;
                }
                ext.position = position;
                WiUpdatePdmNodeQuery UpdateQuery(*node);
                UpdateQuery.updateData = false;
                UpdateQuery.semantic = semantic;
                UpdateQuery.extension = toJson(ext);
                UpdateQuery.updateExtension = true;
                updateNode(initiatingService, UpdateQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
                if(ec) return;
            }else{// if old and new positional are identical
                --depth;
            }
        }
        if(ec) return;

        if(depth == 0) return;
        WiPdmRawNode::Container nodes;
        fetchRawNodes(initiatingService,semantic, nodes, sessionPtr, ec, yield, mctx);
        if(ec) return;
        if(nodes.size() == 0) return;
        {
            auto pos_comparer = DefaultPositionalComparer(oldPos);
            std::sort(nodes.begin(), nodes.end(), pos_comparer);
        }
        pos.push_back(1);
        assignPositionals(initiatingService, pos, nodes, sessionPtr, ec, yield, depth, mctx);
        pos.pop_back();
        return;
    }

    void PdmService::checkElementDestination(
            std::size_t initiatingService,
            const WiAddComponentWithData &query,
            std::int32_t role,
            std::optional<std::int32_t> type,
            bool checkBin,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        boost::ignore_unused(type);
        GUARD_PDM_METHOD();
        std::string destination = query.destination;
        if(query.prepend){
            destination = parentSemantic(query.destination, ec);
            if(ec) return;
        }
        // todo: make more verbose errors

        //check that inside of product
        {
            boost::system::error_code prod_ec;
            nodeNearestAncestor(initiatingService,
                                destination,
                                PdmRoles::Product,
                                0,
                                sessionPtr,
                                prod_ec,
                                yield,
                                mctx);
            if (prod_ec) {
                // check
                if(checkBin) {
                    boost::system::error_code template_ec;
                    nodeNearestAncestor(initiatingService,
                                        destination,
                                        PdmRoles::ProjectElementBin,
                                        0,
                                        sessionPtr,
                                        template_ec,
                                        yield,
                                        mctx);
                    if (template_ec) {
                        ec = template_ec;
                        return;
                    }
                }else{
                    ec = prod_ec;
                    return;
                }
            }
        }
        // get destination node
        auto destination_node = fetchRawNodeEntity(initiatingService,destination,sessionPtr, ec, yield, mctx);
        if(ec || !destination_node) return;
        // depending on destination node role check if node we are trying to add can be here
        switch(destination_node->role){
            // any component can't contain smth inside of it
            case PdmRoles::ElectricComponent:
            case PdmRoles::ProxyComponent: //todo: проверить так ли это - Цыбанов сказал что так может быть в редких случаях.
                ec = make_error_code(error::element_cant_be_added_inside_component);
                return;
            case PdmRoles::BlankComponent:
                if(role != PdmRoles::BlankComponent){
                    ec = make_error_code(error::element_cant_be_added_inside_component);
                    return;
                }
                return;
            case PdmRoles::ProjectElementBin:
                if(!checkBin){
                    ec = make_error_code(error::incorrect_destination);
                    return;
                }
            case PdmRoles::Product:
                // we can add anything into product so just return
                return;
            case PdmRoles::Container:
                if(role == PdmRoles::BlankComponent || role == PdmRoles::ElectricComponent || role == PdmRoles::ProxyComponent){
                    return;
                }
                if(role == PdmRoles::Container){
                    auto prod_node = nodeNearestAncestor(initiatingService, destination_node->semantic, PdmRoles::Product, 0, sessionPtr, ec, yield, mctx);
                    if(ec) return;
                    // calculate depth
                    auto d = semanticDepth(prod_node.semantic, destination_node->semantic);
                    if(9 <= d){
                        ec = make_error_code(error::incorrect_destination);
                        return;
                    }
                }
                else{
                    ec = make_error_code(error::element_invalid);
                    return;
                }
                return;
            default:
                ec = make_error_code(error::incorrect_destination);
                return;
        }
    }

    void PdmService::checkDescendantsDestination(
            std::size_t initiatingService,
            std::shared_ptr<WiPdmRawNodeEntity> node,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        if(!node){
            ec = make_error_code(error::node_not_found);
            return;
        }

        WiAddComponentWithData query;
        query.prepend = false;
        query.destination = node->semantic;

        WiPdmRawNode::Container descendants;
        fetchRawNodes(initiatingService, node->semantic, descendants, sessionPtr, ec, yield,mctx);
        if(ec) return;

        for(auto &desc: descendants){
            checkElementDestination(initiatingService,query,desc.role,desc.type,false,sessionPtr,ec,yield,mctx);
            if(ec) return;
        }
    }

    void PdmService::copyAllDescendantsTo(
            std::size_t initiatingService,
            const std::string &source,
            const std::string &destination,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        auto node = fetchRawNode(initiatingService,source,sessionPtr, ec, yield, mctx);
        if(ec || !node) return;
        if(!node->extension.has_value()){
            ec = make_error_code(error::element_invalid);
            return;
        }
        //todo: refactor positional sorting.
        if(!node->extension.value().contains("position")){
            ec = make_error_code(error::element_invalid);
            return;
        }
        if(!node->extension.value()["position"].is_string()){
            ec = make_error_code(error::element_invalid);
            return;
        }

        WiPdmRawNode::Container nodes;
        fetchRawNodes(initiatingService, source, nodes, sessionPtr, ec, yield, mctx);
        {
            std::vector<std::uint32_t> fp;
            std::string pos = node->extension.value()["position"];
            unwrapPositional(fp, pos);
            std::sort(nodes.begin(), nodes.end(), DefaultPositionalComparer(fp));
        }
        if(nodes.size() == 0) return;


        WiAddElementsToProject query;
        query.prepend = false;
        query.destination = destination;
        for(const auto&desc : nodes){
            query.sources.push_back(desc.semantic);
        }
        copyElementsInternal(initiatingService,query,sessionPtr,ec,yield,mctx);
    }

    std::int32_t PdmService::getPdmComponentRoleOfMdmComponent(
            std::size_t initiatingService,
            const std::string& semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        using namespace mdm::internal;
        switch(MdmSvc.getComponentType(initiatingService, semantic, sessionPtr, ec, yield, mctx)){
            default:
            case MdmComponentType::NONE:
                // todo: error if not ec
                return 0;
                break;
            case MdmComponentType::ELECTRONIC:
                return PdmRoles::ElectricComponent;
                break;
        }
    }


    bool PdmService::updateElementFunctionalUnits(
            std::size_t initiatingService,
            const std::string& prod,
            WiPdmElementData &data,
            const WiUpdateFunctionalUnitsList fus,
            std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        using FUElementType = WiPdmUpdateElementDataQuery::FU_ELEM;
        using FUObjType = FUElementType::ObjectType;
        std::set<std::string> functionalUnits;

        for(const auto &fu:fus){
            std::visit([&](const auto&elem){
                using DecayElementType = typename std::decay<decltype(elem)>::type;
                if constexpr(std::is_same<DecayElementType,std::string>::value){
                    functionalUnits.emplace(elem);
                }else if constexpr(std::is_same<DecayElementType,FUObjType>::value){
                    WiNewFunctionalUnits query;
                    query.semantic = prod;
                    query.elements.emplace_back(elem.header, elem.description);
                    auto res = addFunctionalUnits(initiatingService,query,sessionPtr, ec, yield, mctx);
                    if(ec) return;
                    if(res.has_value()){
                        functionalUnits.emplace(res->semantics.front());
                    }
                }
            },fu);
            if(ec) return false;
        }

        return setFunctionalUnits(initiatingService,data,functionalUnits,FU_updQueries,sessionPtr,ec,yield,mctx);
    }
    bool PdmService::setFunctionalUnits(
            std::size_t initiatingService,
            WiPdmElementData &data,
            const std::set<std::string>& functionalUnits,
            std::pair<std::string, std::reference_wrapper<std::list<WiUpdatePdmNodeQuery>>> FU_updQueries, //out
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(!data.functional_units.has_value()){
            data.functional_units = std::make_optional<std::set<std::string>>();
        }
        bool ret = false;
        auto& fus = data.functional_units.value();
        std::set<std::string> erase_set;
        std::set_difference(fus.begin(), fus.end(), functionalUnits.begin(), functionalUnits.end(),
                            std::inserter(erase_set,erase_set.end()));
        if(erase_set.size()){
            ret = ret || eraseFunctionalUnits(initiatingService,data,erase_set,sessionPtr,ec,yield,mctx);
            if(ec) return ret;

            erasePdmElementFromFunctionalUnits_internal(
                                            initiatingService,
                                            FU_updQueries.first,
                                            erase_set,
                                            FU_updQueries.second,
                                            sessionPtr,
                                            ec,yield,mctx);
            if(ec) return ret;
        }
        std::set<std::string> append_set;
        std::set_difference(functionalUnits.begin(), functionalUnits.end(),fus.begin(), fus.end(),
                            std::inserter(append_set,append_set.end()));
        if(append_set.size()){
            ret = ret || appendFunctionalUnits(initiatingService,data,append_set,sessionPtr,ec,yield,mctx);
            if(ec) return ret;

            appendPdmElementToFunctionalUnits_internal(
                                            initiatingService,
                                            FU_updQueries.first,
                                            append_set,
                                            FU_updQueries.second,
                                            sessionPtr,
                                            ec,yield,mctx);
            if(ec) return ret;
        }
        return ret;
    }

    bool PdmService::appendFunctionalUnits(
            std::size_t initiatingService,
            WiPdmElementData &data,
            const std::set<std::string>& functionalUnits,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        if(!data.functional_units.has_value()){
            data.functional_units = std::make_optional<std::set<std::string>>();
        }
        bool ret = false;
        auto& fus = data.functional_units.value();
        for(const auto& functionalUnit: functionalUnits) {
            if (!fus.count(functionalUnit)) {
                // append
                auto node = fetchRawNode(initiatingService,functionalUnit,sessionPtr, ec, yield, mctx);
                if(!node || ec) return false;
                if(node->role != PdmRoles::FunctionalUnit){
                    ec = make_error_code(error::invalid_node_role);
                    return false;
                }

                fus.insert(functionalUnit);
                ret = true;
            }
        }
        return ret;
    }

    bool PdmService::eraseFunctionalUnits(
            std::size_t initiatingService,
            WiPdmElementData &data,
            const std::set<std::string>& functionalUnits,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(ec,yield,mctx);
        if(!data.functional_units.has_value()){
            data.functional_units = std::make_optional<std::set<std::string>>();
        }
        bool ret = false;
        auto& fus = data.functional_units.value();
        for(const auto& functionalUnit: functionalUnits){
            if(fus.count(functionalUnit)){

                fus.erase(functionalUnit);
                ret = true;
            }
        }
        return ret;
    }

    void PdmService::eraseFunctionalUnitFromElements_internal(
            std::size_t initiatingService,
            const std::string& fu_semantic,
            const std::set<std::string>& elem_semantics,
            std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        std::set<std::string> fuSemantic;
        fuSemantic.emplace(fu_semantic);

        for(const auto& elem_semantic: elem_semantics)
        {
            auto node = fetchRawNodeEntity(initiatingService,elem_semantic,sessionPtr,ec,yield,mctx);
            if(ec) return;

            WiPdmElementData data = fromJson<WiPdmElementData>(node->entity->data.value());
            WiUpdatePdmNodeQuery updateQuery(*node);
            {
                updateQuery.updateData = eraseFunctionalUnits(initiatingService,data,fuSemantic,sessionPtr,ec,yield,mctx);
                if(ec) return;
                if(updateQuery.updateData) {
                    updateQuery.data = toJson(data);
                }
            }
            updateQueries.push_back(updateQuery);
        }
    }

    void PdmService::appendPdmElementToFunctionalUnits_internal(
            std::size_t initiatingService,
            const std::string &elem_semantic,
            const std::set<std::string>& functionalUnits,
            std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        if(!functionalUnits.size()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        for(auto functionalUnit : functionalUnits ){
            updateQueries.emplace_back(appendPdmElementToFunctionalUnit_internal(initiatingService,elem_semantic,functionalUnit,sessionPtr,ec,yield,mctx));
            if(ec) return;
        }

    }

    void PdmService::erasePdmElementFromFunctionalUnits_internal(
            std::size_t initiatingService,
            const std::string &elem_semantic,
            const std::set<std::string>& functionalUnits,
            std::list<WiUpdatePdmNodeQuery>& updateQueries, //out
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();


        if(!functionalUnits.size()){
            ec = make_error_code(error::no_work_to_do);
            return;
        }
        for(auto functionalUnit : functionalUnits ){
            updateQueries.emplace_back(erasePdmElementFromFunctionalUnit_internal(initiatingService,elem_semantic,functionalUnit,sessionPtr,ec,yield,mctx));
            if(ec) return;
        }

    }

    WiUpdatePdmNodeQuery PdmService::appendPdmElementToFunctionalUnit_internal(
            std::size_t initiatingService,
            const std::string &elem_semantic,
            const std::string& functionalUnit,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,functionalUnit,sessionPtr,ec,yield,mctx);
        if(ec) return WiUpdatePdmNodeQuery{};
        switch(node->role){
            case PdmRoles::FunctionalUnit:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return WiUpdatePdmNodeQuery{};
        }

        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return WiUpdatePdmNodeQuery{};
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return WiUpdatePdmNodeQuery{};
        }

        WiPdmFunctionalUnitData data = fromJson<WiPdmFunctionalUnitData>(node->entity->data.value());
        WiUpdatePdmNodeQuery updateQuery(*node);
        {
            if(!data.refs.has_value()){
                data.refs = std::make_optional<std::set<std::string>>();
            }

            auto& refs = data.refs.value();

            if (!refs.count(elem_semantic)) {
                // append
                refs.insert(elem_semantic);
            }

            updateQuery.updateData = true;
            updateQuery.data = toJson(data);
        }
        return updateQuery;
    }


    WiUpdatePdmNodeQuery PdmService::erasePdmElementFromFunctionalUnit_internal(
            std::size_t initiatingService,
            const std::string &elem_semantic,
            const std::string& functionalUnit,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,functionalUnit,sessionPtr,ec,yield,mctx);
        if(ec) return WiUpdatePdmNodeQuery{};
        switch(node->role){
            case PdmRoles::FunctionalUnit:
                break;
            default:
                ec = make_error_code(error::invalid_node_role);
                return WiUpdatePdmNodeQuery{};
        }

        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return WiUpdatePdmNodeQuery{};
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return WiUpdatePdmNodeQuery{};
        }

        WiPdmFunctionalUnitData data = fromJson<WiPdmFunctionalUnitData>(node->entity->data.value());
        WiUpdatePdmNodeQuery updateQuery(*node);
        {
            if(!data.refs.has_value()){
                data.refs = std::make_optional<std::set<std::string>>();
            }


            if (data.refs.value().count(elem_semantic)) {
                // erase
                data.refs.value().erase(elem_semantic);
            }

            updateQuery.updateData = true;
            updateQuery.data = toJson(data);
        }
        return updateQuery;
    }

    std::set<std::string> PdmService::getFunctionalUnitRefs_internal(
            std::size_t initiatingService,
            const std::string &fu_semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,fu_semantic,sessionPtr,ec,yield,mctx);

        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::set<std::string>{};
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::set<std::string>{};
        }

        WiPdmFunctionalUnitData data = fromJson<WiPdmFunctionalUnitData>(node->entity->data.value());

        if(data.refs.has_value()){
            return data.refs.value();
        }
        else{
            return std::set<std::string>{};
        }
    }

    std::set<std::string> PdmService::getFunctionalUnitsOfElement_internal(
            std::size_t initiatingService,
            const std::string &elem_semantic,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code& ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto node = fetchRawNodeEntity(initiatingService,elem_semantic,sessionPtr,ec,yield,mctx);

        if(!node->entity.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::set<std::string>{};
        }
        if(!node->entity->data.has_value()){
            ec = make_error_code(error::element_invalid);
            return std::set<std::string>{};
        }

        WiPdmElementData data = fromJson<WiPdmElementData>(node->entity->data.value());

        if(data.functional_units.has_value()){
            return data.functional_units.value();
        }
        else{
            return std::set<std::string>{};
        }
    }

    WiPdmRawNode PdmService::nodeNearestAncestor(
            std::size_t initiatingService,
            const std::string &semantic,
            std::int32_t role,
            std::int32_t depth,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService);
        boost::ignore_unused(sessionPtr);

        WiPdmRawNode node;
        DataAccessConst().fetchPdmRawNodeNearestAncestor(node, semantic, role, depth, mctx, ec, yield);
        return node;
    }

    WiPdmRawNode PdmService::nodeNearestDescendant(
            std::size_t initiatingService,
            const std::string &semantic,
            std::int32_t role,
            std::int32_t depth,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();
        boost::ignore_unused(initiatingService);
        boost::ignore_unused(sessionPtr);

        WiPdmRawNode node;
        DataAccessConst().fetchPdmRawNodeNearestDescendant(node, semantic, role, depth, mctx, ec, yield);
        return node;
    }

    std::optional<WiSemanticResult> PdmService::createComponentRefInternal(
            std::size_t initiatingService,
            const std::string &semantic,
            const std::string &source,
            const std::shared_ptr<IWiSession> &sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true){
        GUARD_PDM_METHOD();

        checkNode(initiatingService, semantic, PdmRoles::ProjectSettingsComponents, sessionPtr,  ec, yield, mctx);
        if(ec) return std::nullopt;

        // create component refs mirroring mdm structure and
//        std::vector<std::string> sourceSplit;
//        splitSemantic(sourceSplit, source);

        auto cdata = MdmSvcConst.fetchComponentData(initiatingService, source, sessionPtr, ec, yield, mctx);
        if(ec || !cdata.has_value()) return std::nullopt;
        if(!cdata->krr.has_value() || !cdata->ster.has_value()){
            ec = make_error_code(error::component_ref_invalid);
            return std::nullopt;
        }

        WiPdmComponentRefData data;
        data.role = getPdmComponentRoleOfMdmComponent(initiatingService, source, sessionPtr, ec, yield, mctx);
        if(ec) return std::nullopt;
        std::optional<std::int32_t> lang;
        auto mdm_node = MdmSvcConst.fetchNodeView(initiatingService, source, lang, sessionPtr, ec, yield);
        if(ec || !mdm_node) return std::nullopt;

        data.ster = std::move(cdata.value().ster->stereotype);
        data.data = std::move(cdata.value().ster->data);
        data.krr = std::move(cdata.value().krr->stereotype);
        data.env_data = std::move(cdata.value().krr->data);
        WiNewPdmNodeQuery newNodeQuery;

        newNodeQuery.header = std::move(mdm_node.value().name);
        newNodeQuery.description = std::move(mdm_node.value().description);

        newNodeQuery.parent = semantic;
        newNodeQuery.data = toJson(data);
        newNodeQuery.role = PdmRoles::ProjectSettingsComponentsRef;
        auto selfSemantic = addNewNodeInternal(initiatingService, newNodeQuery, sessionPtr, Filter::filterOn, ec, yield, mctx);
        if (ec) {
            return std::nullopt;
        } else {
            WiSemanticResult result;
            result.semantic = selfSemantic;
            return std::make_optional<WiSemanticResult>(std::move(result));
        }
    }

    void PdmService::updateProjectsProjectIdBySystem(
            std::size_t initiatingService,
            uint64_t index,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const
    {
        std::shared_ptr<IWiSession> sessionPtr;
        GUARD_PDM_METHOD();

        auto nodeProjects = fetchRawNodeEntity(initiatingService,H_TO_S(StringHelper::PdmProjectsSemantics),
                                       sessionPtr,
                                       ec,
                                       yield,
                                       mctx);

        if(ec || !nodeProjects) return;

        WiUpdatePdmNodeQuery updateQueryProjects(*nodeProjects);

        if(updateQueryProjects.extension.has_value())
        {
            auto ext = fromJson<WiPdmProjectExtension>(updateQueryProjects.extension.value());
            ext.index = index;
            updateQueryProjects.extension = toJson(ext);
        }
        else
        {
            WiPdmProjectExtension ext;
            ext.index = index;
            updateQueryProjects.extension = toJson(ext);
        }

        updateQueryProjects.updateExtension = true;
        updateQueryProjects.updateData      = false;
        updateQueryProjects.updateRole      = false;
        updateQueryProjects.updateType      = false;

        std::int64_t nodeId = 0;

        //get system as actor
        auto actor = std::make_optional<WiRawActorInfoPart> ({1U, std::chrono::system_clock::now()});

        if(ec) return;

        DataAccessConst().updatePdmNode(
                nodeId,
                updateQueryProjects.semantic,
                updateQueryProjects.role,
                updateQueryProjects.type,
                updateQueryProjects.header,
                updateQueryProjects.description,
                updateQueryProjects.data,
                updateQueryProjects.updateData,
                updateQueryProjects.extension,
                actor,
                mctx,
                ec,
                yield
        );

    }

    std::optional<WiPdmNodeView> PdmService::apply(const WiPdmRawNode &source, boost::system::error_code &ec) const noexcept(true) {
        auto statusResolver = [this](auto &&id, auto &&ec) {
            return getStatus(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        auto fioResolver = [capture = &UsersSvc](auto &&id, auto &&ec) {
            return capture->actorFio(std::forward<decltype(id)>(id), std::forward<decltype(ec)>(ec));
        };

        using StatusResolverType = decltype(statusResolver);
        using FioResolverType = decltype(fioResolver);

        WiPdmNodeView nodeView;
        wi::basic_services::apply<WiPdmStatus, StatusResolverType, FioResolverType>(
                source,
                nodeView,
                m_defaultLanguage,
                ec,
                std::forward<StatusResolverType>(statusResolver),
                std::forward<FioResolverType>(fioResolver));
        if (ec) {
            return std::nullopt;
        }
        return std::make_optional<WiPdmNodeView>(std::move(nodeView));
    }
}
