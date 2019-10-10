#include "pch.h"

#include <vcpkg/base/files.h>
#include <vcpkg/base/graphs.h>
#include <vcpkg/base/strings.h>
#include <vcpkg/base/util.h>
#include <vcpkg/dependencies.h>
#include <vcpkg/packagespec.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/portfileprovider.h>
#include <vcpkg/statusparagraphs.h>
#include <vcpkg/vcpkglib.h>
#include <vcpkg/vcpkgpaths.h>

namespace vcpkg::Dependencies
{
    namespace
    {
        struct ClusterInstalled
        {
            ClusterInstalled(const InstalledPackageView& ipv) : ipv(ipv)
            {
                original_features.emplace("core");
                for (auto&& feature : ipv.features)
                {
                    original_features.emplace(feature->package.feature);
                }
            }

            InstalledPackageView ipv;
            std::unordered_set<PackageSpec> remove_edges;
            std::unordered_set<std::string> original_features;
        };

        struct ClusterInstallInfo
        {
            std::unordered_map<std::string, std::vector<FeatureSpec>> build_edges;
        };

        /// <summary>
        /// Representation of a package and its features in a ClusterGraph.
        /// </summary>
        struct Cluster : Util::MoveOnlyBase
        {
            Cluster(const InstalledPackageView& ipv, const SourceControlFileLocation& scfl)
                : spec(ipv.spec()), scfl(scfl), installed(ipv)
            {
            }

            Cluster(const PackageSpec& spec, const SourceControlFileLocation& scfl) : spec(spec), scfl(scfl) {}

            // Returns dependencies which were added as a result of this call
            std::vector<FeatureSpec> add_feature(const std::string& feature,
                                                 const CMakeVars::CMakeVarProvider& var_provider,
                                                 const Triplet& t)
            {
                std::vector<FeatureSpec> new_dependencies;

                // If install_info is null we have never added a feature which hasn't already been installed to this
                // cluster
                if (!install_info.has_value())
                {
                    if (const ClusterInstalled* inst = installed.get())
                    {
                        auto find_itr = inst->original_features.find(feature);

                        // If this is a new feature add all original features to the brand-new install_info. We need
                        // to rebuild this port and we can't trust the ipv's dependency vectors since the
                        // dependencies of a feature could have changed between runs of vcpkg.
                        if (find_itr == inst->original_features.end())
                        {
                            install_info = make_optional(ClusterInstallInfo{});

                            for (const std::string& installed_feature : inst->original_features)
                            {
                                auto ret = add_feature(installed_feature, var_provider, t);
                                new_dependencies.insert(new_dependencies.end(),
                                                        std::make_move_iterator(ret.begin()),
                                                        std::make_move_iterator(ret.end()));
                            }
                        }
                        else
                        {
                            return {};
                        }
                    }
                    else
                    {
                        install_info = make_optional(ClusterInstallInfo{});
                    }
                }

                ClusterInstallInfo& info = install_info.value_or_exit(VCPKG_LINE_INFO);

                auto maybe_vars = var_provider.get_dep_info_vars(spec);
                const std::unordered_map<std::string, std::string> cmake_vars =
                    maybe_vars.value_or(std::unordered_map<std::string, std::string>{});

                const std::vector<Dependency>* qualified_deps;
                if (feature == "core")
                {
                    qualified_deps = &scfl.source_control_file->core_paragraph->depends;
                }
                else
                {
                    qualified_deps =
                        &scfl.source_control_file->find_feature(feature).value_or_exit(VCPKG_LINE_INFO).depends;
                }

                auto dep_list = filter_dependencies_to_specs(*qualified_deps, t, cmake_vars);
                Util::sort(dep_list);

                // If a change in cmake_vars or the initial addition of this feature results in dependencies being
                // added inform the caller
                auto find_itr = info.build_edges.find(feature);

                if (find_itr == info.build_edges.end())
                {
                    new_dependencies.insert(new_dependencies.end(), dep_list.begin(), dep_list.end());
                    info.build_edges.emplace(feature, std::move(dep_list));
                }
                else
                {
                    std::set_difference(dep_list.begin(),
                                        dep_list.end(),
                                        find_itr->second.begin(),
                                        find_itr->second.end(),
                                        new_dependencies.end());

                    auto old_end = find_itr->second.end();
                    find_itr->second.insert(old_end, new_dependencies.begin(), new_dependencies.end());
                    std::inplace_merge(find_itr->second.begin(), old_end, find_itr->second.end());
                }

                return new_dependencies;
            }

            PackageSpec spec;
            const SourceControlFileLocation& scfl;

            Optional<ClusterInstalled> installed;
            Optional<ClusterInstallInfo> install_info;

            RequestType request_type = RequestType::AUTO_SELECTED;
            bool visited = false;
        };

        struct ClusterPtr
        {
            Cluster* ptr;

            Cluster* operator->() const { return ptr; }
        };

        bool operator==(const ClusterPtr& l, const ClusterPtr& r) { return l.ptr == r.ptr; }
    }
}

namespace std
{
    template<>
    struct hash<vcpkg::Dependencies::ClusterPtr>
    {
        size_t operator()(const vcpkg::Dependencies::ClusterPtr& value) const
        {
            return std::hash<vcpkg::PackageSpec>()(value.ptr->spec);
        }
    };
}

namespace vcpkg::Dependencies
{
    struct GraphPlan
    {
        Graphs::Graph<ClusterPtr> remove_graph;
        Graphs::Graph<ClusterPtr> install_graph;
    };

    /// <summary>
    /// Directional graph representing a collection of packages with their features connected by their dependencies.
    /// </summary>
    struct ClusterGraph : Util::MoveOnlyBase
    {
        explicit ClusterGraph(const PortFileProvider::PortFileProvider& port_provider) : m_port_provider(port_provider)
        {
        }

        /// <summary>
        ///     Find the cluster associated with spec or if not found, create it from the PortFileProvider.
        /// </summary>
        /// <param name="spec">Package spec to get the cluster for.</param>
        /// <returns>The cluster found or created for spec.</returns>
        Cluster& get(const PackageSpec& spec)
        {
            auto it = m_graph.find(spec);
            if (it == m_graph.end())
            {
                const SourceControlFileLocation& scfl =
                    m_port_provider.get_control_file(spec.name()).value_or_exit(VCPKG_LINE_INFO);

                return m_graph
                    .emplace(std::piecewise_construct, std::forward_as_tuple(spec), std::forward_as_tuple(spec, scfl))
                    .first->second;
            }

            return it->second;
        }

        Cluster& get(const InstalledPackageView& ipv)
        {
            auto it = m_graph.find(ipv.spec());

            if (it == m_graph.end())
            {
                Optional<const SourceControlFileLocation&> maybe_scfl =
                    m_port_provider.get_control_file(ipv.spec().name());

                if (!maybe_scfl)
                    Checks::exit_with_message(VCPKG_LINE_INFO,
                                              "We could not find a CONTROL file for ",
                                              ipv.spec().to_string(),
                                              ". Please run \"vcpkg remove ",
                                              ipv.spec().to_string(),
                                              "\" and re-attempt.");

                return m_graph
                    .emplace(std::piecewise_construct,
                             std::forward_as_tuple(ipv.spec()),
                             std::forward_as_tuple(ipv, *maybe_scfl.get()))
                    .first->second;
            }

            if (!it->second.installed)
            {
                it->second.installed = {ipv};
            }

            return it->second;
        }

    private:
        std::unordered_map<PackageSpec, Cluster> m_graph;
        const PortFileProvider::PortFileProvider& m_port_provider;
    };

    static std::string to_output_string(RequestType request_type,
                                        const CStringView s,
                                        const Build::BuildPackageOptions& options,
                                        const fs::path& install_port_path,
                                        const fs::path& default_port_path)
    {
        if (!default_port_path.empty() &&
            !Strings::case_insensitive_ascii_starts_with(install_port_path.u8string(), default_port_path.u8string()))
        {
            const char* const from_head = options.use_head_version == Build::UseHeadVersion::YES ? " (from HEAD)" : "";
            switch (request_type)
            {
                case RequestType::AUTO_SELECTED:
                    return Strings::format("  * %s%s -- %s", s, from_head, install_port_path.u8string());
                case RequestType::USER_REQUESTED:
                    return Strings::format("    %s%s -- %s", s, from_head, install_port_path.u8string());
                default: Checks::unreachable(VCPKG_LINE_INFO);
            }
        }
        return to_output_string(request_type, s, options);
    }

    std::string to_output_string(RequestType request_type,
                                 const CStringView s,
                                 const Build::BuildPackageOptions& options)
    {
        const char* const from_head = options.use_head_version == Build::UseHeadVersion::YES ? " (from HEAD)" : "";

        switch (request_type)
        {
            case RequestType::AUTO_SELECTED: return Strings::format("  * %s%s", s, from_head);
            case RequestType::USER_REQUESTED: return Strings::format("    %s%s", s, from_head);
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    std::string to_output_string(RequestType request_type, const CStringView s)
    {
        switch (request_type)
        {
            case RequestType::AUTO_SELECTED: return Strings::format("  * %s", s);
            case RequestType::USER_REQUESTED: return Strings::format("    %s", s);
            default: Checks::unreachable(VCPKG_LINE_INFO);
        }
    }

    InstallPlanAction::InstallPlanAction() noexcept
        : plan_type(InstallPlanType::UNKNOWN), request_type(RequestType::UNKNOWN), build_options{}
    {
    }

    InstallPlanAction::InstallPlanAction(const PackageSpec& spec,
                                         const SourceControlFileLocation& scfl,
                                         const RequestType& request_type,
                                         std::unordered_map<std::string, std::vector<FeatureSpec>>&& dependencies)
        : spec(spec)
        , source_control_file_location(scfl)
        , plan_type(InstallPlanType::BUILD_AND_INSTALL)
        , request_type(request_type)
        , build_options{}
        , feature_dependencies(std::move(dependencies))
    {
        for (const auto& kv : feature_dependencies)
        {
            feature_list.emplace_back(kv.first);
            for (const FeatureSpec& fspec : kv.second)
            {
                if (spec != fspec.spec())
                {
                    package_dependencies.emplace_back(fspec.spec());
                }
            }
        }

        Util::sort_unique_erase(package_dependencies);
    }

    InstallPlanAction::InstallPlanAction(InstalledPackageView&& ipv, const RequestType& request_type)
        : spec(ipv.spec())
        , installed_package(std::move(ipv))
        , plan_type(InstallPlanType::ALREADY_INSTALLED)
        , request_type(request_type)
        , build_options{}
        , feature_dependencies(installed_package.get()->feature_dependencies())
        , package_dependencies(installed_package.get()->dependencies())
    {
        for (const auto& kv : feature_dependencies)
        {
            feature_list.emplace_back(kv.first);
        }
    }

    std::string InstallPlanAction::displayname() const
    {
        if (this->feature_dependencies.empty())
        {
            return this->spec.to_string();
        }

        const std::string features =
            Strings::join(",",
                          Util::fmap(feature_dependencies,
                                     [](const decltype(feature_dependencies)::value_type& kv) { return kv.first; }));
        return Strings::format("%s[%s]:%s", this->spec.name(), features, this->spec.triplet());
    }

    bool InstallPlanAction::compare_by_name(const InstallPlanAction* left, const InstallPlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    RemovePlanAction::RemovePlanAction() noexcept
        : plan_type(RemovePlanType::UNKNOWN), request_type(RequestType::UNKNOWN)
    {
    }

    RemovePlanAction::RemovePlanAction(const PackageSpec& spec,
                                       const RemovePlanType& plan_type,
                                       const RequestType& request_type)
        : spec(spec), plan_type(plan_type), request_type(request_type)
    {
    }

    const PackageSpec& AnyAction::spec() const
    {
        if (const auto p = install_action.get())
        {
            return p->spec;
        }

        if (const auto p = remove_action.get())
        {
            return p->spec;
        }

        Checks::exit_with_message(VCPKG_LINE_INFO, "Null action");
    }

    bool ExportPlanAction::compare_by_name(const ExportPlanAction* left, const ExportPlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    ExportPlanAction::ExportPlanAction() noexcept
        : plan_type(ExportPlanType::UNKNOWN), request_type(RequestType::UNKNOWN)
    {
    }

    ExportPlanAction::ExportPlanAction(const PackageSpec& spec,
                                       InstalledPackageView&& installed_package,
                                       const RequestType& request_type)
        : spec(spec)
        , plan_type(ExportPlanType::ALREADY_BUILT)
        , request_type(request_type)
        , m_installed_package(std::move(installed_package))
    {
    }

    ExportPlanAction::ExportPlanAction(const PackageSpec& spec, const RequestType& request_type)
        : spec(spec), plan_type(ExportPlanType::NOT_BUILT), request_type(request_type)
    {
    }

    Optional<const BinaryParagraph&> ExportPlanAction::core_paragraph() const
    {
        if (auto p_ip = m_installed_package.get())
        {
            return p_ip->core->package;
        }
        return nullopt;
    }

    std::vector<PackageSpec> ExportPlanAction::dependencies(const Triplet&) const
    {
        if (auto p_ip = m_installed_package.get())
            return p_ip->dependencies();
        else
            return {};
    }

    bool RemovePlanAction::compare_by_name(const RemovePlanAction* left, const RemovePlanAction* right)
    {
        return left->spec.name() < right->spec.name();
    }

    std::vector<RemovePlanAction> PackageGraph::create_remove_plan(const std::vector<PackageSpec>& specs,
                                                                   const StatusParagraphs& status_db)
    {
        struct RemoveAdjacencyProvider final : Graphs::AdjacencyProvider<PackageSpec, RemovePlanAction>
        {
            const StatusParagraphs& status_db;
            const std::vector<InstalledPackageView>& installed_ports;
            const std::unordered_set<PackageSpec>& specs_as_set;

            RemoveAdjacencyProvider(const StatusParagraphs& status_db,
                                    const std::vector<InstalledPackageView>& installed_ports,
                                    const std::unordered_set<PackageSpec>& specs_as_set)
                : status_db(status_db), installed_ports(installed_ports), specs_as_set(specs_as_set)
            {
            }

            std::vector<PackageSpec> adjacency_list(const RemovePlanAction& plan) const override
            {
                if (plan.plan_type == RemovePlanType::NOT_INSTALLED)
                {
                    return {};
                }

                const PackageSpec& spec = plan.spec;
                std::vector<PackageSpec> dependents;
                for (auto&& ipv : installed_ports)
                {
                    auto deps = ipv.dependencies();

                    if (std::find(deps.begin(), deps.end(), spec) == deps.end()) continue;

                    dependents.push_back(ipv.spec());
                }

                return dependents;
            }

            RemovePlanAction load_vertex_data(const PackageSpec& spec) const override
            {
                const RequestType request_type = specs_as_set.find(spec) != specs_as_set.end()
                                                     ? RequestType::USER_REQUESTED
                                                     : RequestType::AUTO_SELECTED;
                const StatusParagraphs::const_iterator it = status_db.find_installed(spec);
                if (it == status_db.end())
                {
                    return RemovePlanAction{spec, RemovePlanType::NOT_INSTALLED, request_type};
                }
                return RemovePlanAction{spec, RemovePlanType::REMOVE, request_type};
            }

            std::string to_string(const PackageSpec& spec) const override { return spec.to_string(); }
        };

        auto installed_ports = get_installed_ports(status_db);
        const std::unordered_set<PackageSpec> specs_as_set(specs.cbegin(), specs.cend());
        return Graphs::topological_sort(specs, RemoveAdjacencyProvider{status_db, installed_ports, specs_as_set}, {});
    }

    std::vector<ExportPlanAction> PackageGraph::create_export_plan(const std::vector<PackageSpec>& specs,
                                                                   const StatusParagraphs& status_db)
    {
        struct ExportAdjacencyProvider final : Graphs::AdjacencyProvider<PackageSpec, ExportPlanAction>
        {
            const StatusParagraphs& status_db;
            const std::unordered_set<PackageSpec>& specs_as_set;

            ExportAdjacencyProvider(const StatusParagraphs& s, const std::unordered_set<PackageSpec>& specs_as_set)
                : status_db(s), specs_as_set(specs_as_set)
            {
            }

            std::vector<PackageSpec> adjacency_list(const ExportPlanAction& plan) const override
            {
                return plan.dependencies(plan.spec.triplet());
            }

            ExportPlanAction load_vertex_data(const PackageSpec& spec) const override
            {
                const RequestType request_type = specs_as_set.find(spec) != specs_as_set.end()
                                                     ? RequestType::USER_REQUESTED
                                                     : RequestType::AUTO_SELECTED;

                auto maybe_ipv = status_db.find_all_installed(spec);

                if (auto p_ipv = maybe_ipv.get())
                {
                    return ExportPlanAction{spec, std::move(*p_ipv), request_type};
                }

                return ExportPlanAction{spec, request_type};
            }

            std::string to_string(const PackageSpec& spec) const override { return spec.to_string(); }
        };

        const std::unordered_set<PackageSpec> specs_as_set(specs.cbegin(), specs.cend());
        std::vector<ExportPlanAction> toposort =
            Graphs::topological_sort(specs, ExportAdjacencyProvider{status_db, specs_as_set}, {});
        return toposort;
    }

    void PackageGraph::mark_user_requested(const PackageSpec& spec)
    {
        m_graph->get(spec).request_type = RequestType::USER_REQUESTED;
    }

    std::vector<AnyAction> PackageGraph::create_feature_install_plan(
        const PortFileProvider::PortFileProvider& port_provider,
        const CMakeVars::CMakeVarProvider& var_provider,
        const std::vector<FullPackageSpec>& specs,
        const StatusParagraphs& status_db,
        const CreateInstallPlanOptions& options)
    {
        PackageGraph pgraph(port_provider, var_provider, status_db);

        std::vector<FeatureSpec> feature_specs;
        for (const FullPackageSpec& spec : specs)
        {
            const SourceControlFileLocation& scfl =
                port_provider.get_control_file(spec.package_spec.name()).value_or_exit(VCPKG_LINE_INFO);
            auto fspecs =
                FullPackageSpec::to_feature_specs(spec, scfl.source_control_file->core_paragraph->default_features);
            feature_specs.insert(
                feature_specs.end(), std::make_move_iterator(fspecs.begin()), std::make_move_iterator(fspecs.end()));
        }
        Util::sort_unique_erase(feature_specs);

        pgraph.install(feature_specs);
        for (const FeatureSpec& spec : feature_specs)
        {
            pgraph.mark_user_requested(spec.spec());
        }

        return pgraph.serialize(options);
    }

    std::vector<FeatureSpec> PackageGraph::get_required_removals(const PackageSpec& first_remove_spec) const
    {
        std::vector<PackageSpec> to_remove{first_remove_spec};
        std::vector<FeatureSpec> removed;

        while (!to_remove.empty())
        {
            PackageSpec remove_spec = std::move(to_remove.back());
            to_remove.pop_back();

            Cluster& clust = m_graph->get(remove_spec);
            ClusterInstalled& info = clust.installed.value_or_exit(VCPKG_LINE_INFO);

            m_graph_plan->remove_graph.add_vertex({&clust});

            for (const std::string& orig_feature : info.original_features)
            {
                removed.emplace_back(remove_spec, orig_feature);
            }

            for (const PackageSpec& new_remove_spec : info.remove_edges)
            {
                Cluster& depend_cluster = m_graph->get(new_remove_spec);
                if (!depend_cluster.install_info)
                {
                    depend_cluster.install_info = make_optional(ClusterInstallInfo{});
                    to_remove.emplace_back(new_remove_spec);
                }

                m_graph_plan->remove_graph.add_edge({&clust}, {&depend_cluster});
            }
        }

        return removed;
    }

    void PackageGraph::install(Span<const FeatureSpec> specs) const
    {
        std::vector<FeatureSpec> qualified_specs;
        std::vector<FeatureSpec> next_dependencies{specs.begin(), specs.end()};

        // Mark all the clusters that are explicitly requested as visited so we don't add default features later
        for (const FeatureSpec& explicit_spec : specs)
        {
            Cluster& clust = m_graph->get(explicit_spec.spec());
            if (!clust.visited)
            {
                clust.visited = true;
                m_graph_plan->install_graph.add_vertex({&clust});
            }
        }

        // Keep running while there is any chance of finding more dependencies
        while (!next_dependencies.empty())
        {
            // Keep running until the only dependencies left are qualified
            while (!next_dependencies.empty())
            {
                // Extract the top of the stack
                FeatureSpec spec = std::move(next_dependencies.back());
                next_dependencies.pop_back();

                // Get the cluster for the PackageSpec of the FeatureSpec we are adding to the install graph
                Cluster& clust = m_graph->get(spec.spec());

                // TODO: There's always the chance that we don't find the feature we're looking for (probably a
                // malformed CONTROL file somewhere). We should probably output a better error.
                const std::vector<Dependency>* paragraph_depends;
                if (spec.feature() == "core")
                {
                    paragraph_depends = &clust.scfl.source_control_file->core_paragraph->depends;
                }
                else
                {
                    paragraph_depends = &clust.scfl.source_control_file->find_feature(spec.feature())
                                             .value_or_exit(VCPKG_LINE_INFO)
                                             .depends;
                }

                for (const Dependency& dep : *paragraph_depends)
                {
                    // We need to keep track of qualified dependencies so that we can re-add to the cluster with the
                    // updated CMakeVarProvider. We do this to load the triplet values in as few discrete calls as
                    // possible. Every time we load a list of ports it's an invocation of CMake, which takes ~150ms
                    // per call.
                    if (!dep.qualifier.empty() && !m_var_provider.get_dep_info_vars(spec.spec()).has_value())
                    {
                        qualified_specs.emplace_back(spec);
                    }
                }

                bool port_installed = clust.installed.has_value();
                bool build_needed = clust.install_info.has_value();
                auto new_dependencies = clust.add_feature(spec.feature(), m_var_provider, spec.triplet());

                // If the port was already installed and this is the first time we're adding features then we're
                // going to need to transiently uninstall it. Checking that the port is already installed and adding
                // a feature resulted in more new dependencies is insufficient since a feature can have no
                // dependencies.
                if (port_installed && !build_needed && clust.install_info.has_value())
                {
                    auto reinstall_features = get_required_removals(spec.spec());
                    next_dependencies.insert(next_dependencies.end(),
                                             std::make_move_iterator(reinstall_features.begin()),
                                             std::make_move_iterator(reinstall_features.end()));
                }

                // Create graph vertices for each of our dependencies and create an edge from us to each of our
                // dependencies. If our dependency's cluster hasn't been visited in the past, add its default
                // features. We assume the first time we visit a cluster is when we decide to add the default
                // features or not. For a feature with qualified dependencies we can enter the body of this loop up
                // to twice. Once to collect all the unqualified dependencies and once after we've run the triplet
                // to collect dependency information for qualified dependencies.
                for (const FeatureSpec& dep_spec : new_dependencies)
                {
                    Cluster& dep_clust = m_graph->get(dep_spec.spec());

                    if (!dep_clust.visited)
                    {
                        dep_clust.visited = true;
                        m_graph_plan->install_graph.add_vertex({&dep_clust});

                        // Add default features
                        for (const std::string& feature :
                             dep_clust.scfl.source_control_file->core_paragraph->default_features)
                        {
                            // Instead of dealing with adding default features to each of our dependencies right
                            // away we just defer to the next pass of the loop.
                            next_dependencies.emplace_back(dep_spec.spec(), feature);
                        }

                        next_dependencies.emplace_back(dep_spec.spec(), "core");
                    }

                    // Note that we don't need to run this for each of the default dependencies since they all
                    // belong to the same cluster as the feature dependency.
                    if (dep_spec.spec() != clust.spec)
                    {
                        m_graph_plan->install_graph.add_edge({&clust}, {&dep_clust});
                    }
                }

                next_dependencies.insert(next_dependencies.end(),
                                         std::make_move_iterator(new_dependencies.begin()),
                                         std::make_move_iterator(new_dependencies.end()));
            }

            if (!qualified_specs.empty())
            {
                // Extract the package specs we need to get dependency info from. We don't run the triplet on a per
                // feature basis. We run it once for the whole port.
                auto qualified_package_specs =
                    Util::fmap(qualified_specs, [](const FeatureSpec& fspec) { return fspec.spec(); });
                Util::sort_unique_erase(qualified_package_specs);
                m_var_provider.load_dep_info_vars(qualified_package_specs);

                // At a glance qualified_specs shouldn't have any duplicates but I don't have a proof, so we
                // sort_unique_erase.
                Util::sort_unique_erase(qualified_specs);

                // Put all the FeatureSpecs for which we had qualified dependencies back on the dependencies stack.
                // We need to recheck if evaluating the triplet revealed any new dependencies.
                next_dependencies.insert(next_dependencies.end(),
                                         std::make_move_iterator(qualified_specs.begin()),
                                         std::make_move_iterator(qualified_specs.end()));
                qualified_specs.clear();
            }
        }
    }

    void PackageGraph::upgrade(Span<const PackageSpec> specs) const
    {
        std::vector<FeatureSpec> removals;

        for (const PackageSpec& spec : specs)
        {
            auto specific_removals = get_required_removals(spec);
            removals.insert(removals.end(),
                            std::make_move_iterator(specific_removals.begin()),
                            std::make_move_iterator(specific_removals.end()));
            m_graph->get(spec).request_type = RequestType::USER_REQUESTED;
        }

        Util::sort_unique_erase(removals);

        install(removals);
    }

    std::vector<AnyAction> PackageGraph::create_upgrade_plan(const PortFileProvider::PortFileProvider& port_provider,
                                                             const CMakeVars::CMakeVarProvider& var_provider,
                                                             const std::vector<PackageSpec>& specs,
                                                             const StatusParagraphs& status_db,
                                                             const CreateInstallPlanOptions& options)
    {
        PackageGraph pgraph(port_provider, var_provider, status_db);

        pgraph.upgrade(specs);

        return pgraph.serialize(options);
    }

    std::vector<AnyAction> PackageGraph::serialize(const CreateInstallPlanOptions& options) const
    {
        auto remove_vertex_list = m_graph_plan->remove_graph.vertex_list();
        auto remove_toposort =
            Graphs::topological_sort(remove_vertex_list, m_graph_plan->remove_graph, options.randomizer);

        auto insert_vertex_list = m_graph_plan->install_graph.vertex_list();
        auto insert_toposort =
            Graphs::topological_sort(insert_vertex_list, m_graph_plan->install_graph, options.randomizer);

        std::vector<AnyAction> plan;

        for (auto&& p_cluster : remove_toposort)
        {
            plan.emplace_back(RemovePlanAction{
                std::move(p_cluster->spec),
                RemovePlanType::REMOVE,
                p_cluster->request_type,
            });
        }

        for (auto&& p_cluster : insert_toposort)
        {
            // Every cluster that has an install_info needs to be built
            // If a cluster only has an installed object and is marked as user requested we should still report it.
            if (auto info_ptr = p_cluster->install_info.get())
            {
                auto&& scfl = p_cluster->scfl;

                plan.emplace_back(InstallPlanAction{
                    p_cluster->spec, scfl, p_cluster->request_type, std::move(info_ptr->build_edges)});
            }
            else if (p_cluster->request_type == RequestType::USER_REQUESTED && p_cluster->installed.has_value())
            {
                auto&& installed = p_cluster->installed.value_or_exit(VCPKG_LINE_INFO);
                plan.emplace_back(InstallPlanAction{
                    std::move(installed.ipv),
                    p_cluster->request_type,
                });
            }
        }

        return plan;
    }

    static std::unique_ptr<ClusterGraph> create_feature_install_graph(
        const PortFileProvider::PortFileProvider& port_provider, const StatusParagraphs& status_db)
    {
        std::unique_ptr<ClusterGraph> graph = std::make_unique<ClusterGraph>(port_provider);

        auto installed_ports = get_installed_ports(status_db);

        for (auto&& ipv : installed_ports)
        {
            graph->get(ipv);
        }

        // Populate the graph with "remove edges", which are the reverse of the Build-Depends edges.
        for (auto&& ipv : installed_ports)
        {
            auto deps = ipv.dependencies();

            for (auto&& dep : deps)
            {
                auto p_installed = graph->get(dep).installed.get();
                Checks::check_exit(VCPKG_LINE_INFO,
                                   p_installed,
                                   "Error: database corrupted. Package %s is installed but dependency %s is not.",
                                   ipv.spec(),
                                   dep);
                p_installed->remove_edges.emplace(ipv.spec());
            }
        }
        return graph;
    }

    PackageGraph::PackageGraph(const PortFileProvider::PortFileProvider& port_provider,
                               const CMakeVars::CMakeVarProvider& var_provider,
                               const StatusParagraphs& status_db)
        : m_var_provider(var_provider)
        , m_graph_plan(std::make_unique<GraphPlan>())
        , m_graph(create_feature_install_graph(port_provider, status_db))
    {
    }

    PackageGraph::~PackageGraph() = default;

    void print_plan(const std::vector<AnyAction>& action_plan,
                    const bool is_recursive,
                    const fs::path& default_ports_dir)
    {
        std::vector<const RemovePlanAction*> remove_plans;
        std::vector<const InstallPlanAction*> rebuilt_plans;
        std::vector<const InstallPlanAction*> only_install_plans;
        std::vector<const InstallPlanAction*> new_plans;
        std::vector<const InstallPlanAction*> already_installed_plans;
        std::vector<const InstallPlanAction*> excluded;

        const bool has_non_user_requested_packages = Util::find_if(action_plan, [](const AnyAction& package) -> bool {
                                                         if (auto iplan = package.install_action.get())
                                                             return iplan->request_type != RequestType::USER_REQUESTED;
                                                         else
                                                             return false;
                                                     }) != action_plan.cend();

        for (auto&& action : action_plan)
        {
            if (auto install_action = action.install_action.get())
            {
                // remove plans are guaranteed to come before install plans, so we know the plan will be contained
                // if at all.
                auto it = Util::find_if(
                    remove_plans, [&](const RemovePlanAction* plan) { return plan->spec == install_action->spec; });
                if (it != remove_plans.end())
                {
                    rebuilt_plans.emplace_back(install_action);
                }
                else
                {
                    switch (install_action->plan_type)
                    {
                        case InstallPlanType::ALREADY_INSTALLED:
                            if (install_action->request_type == RequestType::USER_REQUESTED)
                                already_installed_plans.emplace_back(install_action);
                            break;
                        case InstallPlanType::BUILD_AND_INSTALL: new_plans.emplace_back(install_action); break;
                        case InstallPlanType::EXCLUDED: excluded.emplace_back(install_action); break;
                        default: Checks::unreachable(VCPKG_LINE_INFO);
                    }
                }
            }
            else if (auto remove_action = action.remove_action.get())
            {
                remove_plans.emplace_back(remove_action);
            }
        }

        std::sort(remove_plans.begin(), remove_plans.end(), &RemovePlanAction::compare_by_name);
        std::sort(rebuilt_plans.begin(), rebuilt_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(only_install_plans.begin(), only_install_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(new_plans.begin(), new_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(already_installed_plans.begin(), already_installed_plans.end(), &InstallPlanAction::compare_by_name);
        std::sort(excluded.begin(), excluded.end(), &InstallPlanAction::compare_by_name);

        static auto actions_to_output_string = [&](const std::vector<const InstallPlanAction*>& v) {
            return Strings::join("\n", v, [&](const InstallPlanAction* p) {
                if (auto* pscfl = p->source_control_file_location.get())
                {
                    return to_output_string(
                        p->request_type, p->displayname(), p->build_options, pscfl->source_location, default_ports_dir);
                }

                return to_output_string(p->request_type, p->displayname(), p->build_options);
            });
        };

        if (!excluded.empty())
        {
            System::print2("The following packages are excluded:\n", actions_to_output_string(excluded), '\n');
        }

        if (!already_installed_plans.empty())
        {
            System::print2("The following packages are already installed:\n",
                           actions_to_output_string(already_installed_plans),
                           '\n');
        }

        if (!rebuilt_plans.empty())
        {
            System::print2("The following packages will be rebuilt:\n", actions_to_output_string(rebuilt_plans), '\n');
        }

        if (!new_plans.empty())
        {
            System::print2(
                "The following packages will be built and installed:\n", actions_to_output_string(new_plans), '\n');
        }

        if (!only_install_plans.empty())
        {
            System::print2("The following packages will be directly installed:\n",
                           actions_to_output_string(only_install_plans),
                           '\n');
        }

        if (has_non_user_requested_packages)
            System::print2("Additional packages (*) will be modified to complete this operation.\n");

        if (!remove_plans.empty() && !is_recursive)
        {
            System::print2(System::Color::warning,
                           "If you are sure you want to rebuild the above packages, run the command with the "
                           "--recurse option\n");
            Checks::exit_fail(VCPKG_LINE_INFO);
        }
    }
}
