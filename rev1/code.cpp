void PdmService::getRbdChainSemantics(
            std::size_t initiatingService,
            std::vector<std::string>& copies,
            const WiRbdChain &chain,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto start_node = fetchRawNodeEntity(initiatingService,chain.source,sessionPtr, ec, yield, mctx);
        if (ec || !start_node) return;

        auto end_node = fetchRawNodeEntity(initiatingService,chain.target,sessionPtr, ec, yield, mctx);
        if (ec || !end_node) return;

        do
        {
            if (start_node->role == PdmRoles::RbdBlock || start_node->role == PdmRoles::SubRbd)
            {
                copies.push_back(start_node->semantic);
            }
            else if (start_node->role == PdmRoles::RbdGroupStart)
            {
                auto group = getRbdGroup(initiatingService,start_node->semantic,sessionPtr, ec, yield, mctx);
                if (ec || !group.has_value()) return;

                getRbdGroupSemantics(initiatingService, copies, group.value(), sessionPtr, ec, yield, mctx);
                if (ec) return;

                start_node = fetchRawNodeEntity(initiatingService,group.value().target,sessionPtr, ec, yield, mctx);
                if (ec) return;
            }

            // end loop condition
            if(start_node->semantic == end_node->semantic)
                break;

            std::optional<std::string> output;
            getRbdElementOutput(initiatingService, start_node, output, sessionPtr, ec, yield, mctx);
            if (ec) return;

            if (!output.has_value()) {
                ec = make_error_code(error::invalid_rbd_element);
                return;
            }

            start_node = fetchRawNodeEntity(initiatingService,output.value(),sessionPtr, ec, yield, mctx);
            if (ec) return;

        } while(true);
    }

    void PdmService::getRbdGroupSemantics(
            std::size_t initiatingService,
            std::vector<std::string>& copies,
            const WiRbdChain &group,
            const std::shared_ptr<IWiSession> sessionPtr,
            boost::system::error_code &ec,
            const net::yield_context &yield,
            std::shared_ptr<MethodContextInterface> ctx) const noexcept(true)
    {
        GUARD_PDM_METHOD();

        auto start_node = fetchRawNodeEntity(initiatingService,group.source,sessionPtr,ec,yield,mctx);
        if(ec || !start_node) return;

        if(start_node->role != PdmRoles::RbdGroupStart){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        auto end_node = fetchRawNodeEntity(initiatingService,group.target,sessionPtr,ec,yield,mctx);
        if(ec || !end_node) return;

        if(end_node->role != PdmRoles::RbdGroupEnd){
            ec = make_error_code(error::invalid_rbd_element);
            return;
        }

        std::vector<std::string> orig_outputs;
        getRbdElementOutputs(initiatingService,start_node,orig_outputs,sessionPtr,ec,yield,mctx);
        if(ec) return;

        for(const auto& orig_output: orig_outputs)
        {
            WiRbdChain orig_chain;
            orig_chain.source = orig_output;
            orig_chain.target = group.target;

            getRbdChainSemantics(initiatingService, copies, orig_chain,sessionPtr,ec,yield,mctx);
            if (ec) return;
        }
    }
