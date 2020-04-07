// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "WorkflowBase.h"
#include "ExecutionContext.h"
#include "ManifestComparator.h"


namespace AppInstaller::CLI::Workflow
{
    using namespace AppInstaller::Repository;

    bool WorkflowTask::operator==(const WorkflowTask& other) const
    {
        if (m_isFunc && other.m_isFunc)
        {
            return m_func == other.m_func;
        }
        else if (!m_isFunc && !other.m_isFunc)
        {
            return m_name == other.m_name;
        }
        else
        {
            return false;
        }
    }

    void WorkflowTask::operator()(Execution::Context& context) const
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_isFunc);
        m_func(context);
    }

    void OpenSource(Execution::Context& context)
    {
        std::string_view sourceName;
        if (context.Args.Contains(Execution::Args::Type::Source))
        {
            sourceName = context.Args.GetArg(Execution::Args::Type::Source);
        }

        std::shared_ptr<Repository::ISource> source = context.Reporter.ExecuteWithProgress(std::bind(Repository::OpenSource, sourceName, std::placeholders::_1));

        if (!source)
        {
            std::vector<SourceDetails> sources = GetSources();

            if (context.Args.Contains(Execution::Args::Type::Source) && !sources.empty())
            {
                // A bad name was given, try to help.
                context.Reporter.Error() << "No sources match the given value: " << sourceName << std::endl;
                context.Reporter.Info() << "The configured sources are:" << std::endl;
                for (const auto& details : sources)
                {
                    context.Reporter.Info() << "  " << details.Name << std::endl;
                }

                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_SOURCE_NAME_DOES_NOT_EXIST);
            }
            else
            {
                // Even if a name was given, there are no sources
                context.Reporter.Error() << "No sources defined; add one with 'source add'" << std::endl;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_NO_SOURCES_DEFINED);
            }
        }

        context.Add<Execution::Data::Source>(std::move(source));
    }

    void SearchSource(Execution::Context& context)
    {
        auto& args = context.Args;

        // Construct query
        MatchType matchType = MatchType::Substring;
        if (args.Contains(Execution::Args::Type::Exact))
        {
            matchType = MatchType::Exact;
        }

        SearchRequest searchRequest;
        if (args.Contains(Execution::Args::Type::Query))
        {
            searchRequest.Query.emplace(RequestMatch(matchType, args.GetArg(Execution::Args::Type::Query)));
        }

        if (args.Contains(Execution::Args::Type::Id))
        {
            searchRequest.Filters.emplace_back(ApplicationMatchFilter(ApplicationMatchField::Id, matchType, args.GetArg(Execution::Args::Type::Id)));
        }

        if (args.Contains(Execution::Args::Type::Name))
        {
            searchRequest.Filters.emplace_back(ApplicationMatchFilter(ApplicationMatchField::Name, matchType, args.GetArg(Execution::Args::Type::Name)));
        }

        if (args.Contains(Execution::Args::Type::Moniker))
        {
            searchRequest.Filters.emplace_back(ApplicationMatchFilter(ApplicationMatchField::Moniker, matchType, args.GetArg(Execution::Args::Type::Moniker)));
        }

        if (args.Contains(Execution::Args::Type::Tag))
        {
            searchRequest.Filters.emplace_back(ApplicationMatchFilter(ApplicationMatchField::Tag, matchType, args.GetArg(Execution::Args::Type::Tag)));
        }

        if (args.Contains(Execution::Args::Type::Command))
        {
            searchRequest.Filters.emplace_back(ApplicationMatchFilter(ApplicationMatchField::Command, matchType, args.GetArg(Execution::Args::Type::Command)));
        }

        if (args.Contains(Execution::Args::Type::Count))
        {
            searchRequest.MaximumResults = std::stoi(std::string(args.GetArg(Execution::Args::Type::Count)));
        }

        Logging::Telemetry().LogSearchRequest(
            args.GetArg(Execution::Args::Type::Query),
            args.GetArg(Execution::Args::Type::Id),
            args.GetArg(Execution::Args::Type::Name),
            args.GetArg(Execution::Args::Type::Moniker),
            args.GetArg(Execution::Args::Type::Tag),
            args.GetArg(Execution::Args::Type::Command),
            searchRequest.MaximumResults,
            searchRequest.ToString());

        context.Add<Execution::Data::SearchResult>(context.Get<Execution::Data::Source>()->Search(searchRequest));
    }

    void ReportSearchResult(Execution::Context& context)
    {
        auto& searchResult = context.Get<Execution::Data::SearchResult>();
        Logging::Telemetry().LogSearchResultCount(searchResult.Matches.size());
        for (auto& match : searchResult.Matches)
        {
            auto app = match.Application.get();
            auto allVersions = app->GetVersions();

            // Assume versions are sorted when returned so we'll use the first one as the latest version
            context.Reporter.Info() << app->GetId() << ", " << app->GetName() << ", " << allVersions.at(0).GetVersion().ToString();

            if (match.MatchCriteria.Field != ApplicationMatchField::Id && match.MatchCriteria.Field != ApplicationMatchField::Name)
            {
                context.Reporter.Info() << ", [" << ApplicationMatchFieldToString(match.MatchCriteria.Field) << ": " << match.MatchCriteria.Value << "]";
            }

            context.Reporter.Info() << std::endl;
        }
    }

    void EnsureMatchesFromSearchResult(Execution::Context& context)
    {
        auto& searchResult = context.Get<Execution::Data::SearchResult>();

        if (searchResult.Matches.size() == 0)
        {
            Logging::Telemetry().LogNoAppMatch();
            context.Reporter.Info() << "No app found matching input criteria." << std::endl;
            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_NO_APPLICATIONS_FOUND);
        }
    }

    void EnsureOneMatchFromSearchResult(Execution::Context& context)
    {
        context <<
            EnsureMatchesFromSearchResult <<
            [](Execution::Context& context)
        {
            auto& searchResult = context.Get<Execution::Data::SearchResult>();

            if (searchResult.Matches.size() > 1)
            {
                Logging::Telemetry().LogMultiAppMatch();
                context.Reporter.Warn() << "Multiple apps found matching input criteria. Please refine the input." << std::endl;
                context << ReportSearchResult;
                AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_MULTIPLE_APPLICATIONS_FOUND);
            }

            auto app = searchResult.Matches.at(0).Application.get();
            Logging::Telemetry().LogAppFound(app->GetName(), app->GetId());
        };
    }

    void GetManifestFromSearchResult(Execution::Context& context)
    {
        auto app = context.Get<Execution::Data::SearchResult>().Matches.at(0).Application.get();

        std::string_view version = context.Args.GetArg(Execution::Args::Type::Version);
        std::string_view channel = context.Args.GetArg(Execution::Args::Type::Channel);

        std::optional<Manifest::Manifest> manifest = app->GetManifest(version, channel);

        if (!manifest)
        {
            context.Reporter.Error() << "No version found matching: ";
            if (!version.empty())
            {
                context.Reporter.Error() << version;
            }
            if (!channel.empty())
            {
                context.Reporter.Error() << '[' << channel << ']';
            }

            context.Reporter.Error() << std::endl;
            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_NO_MANIFEST_FOUND);
        }

        Logging::Telemetry().LogManifestFields(manifest->Id, manifest->Name, manifest->Version);
        context.Add<Execution::Data::Manifest>(std::move(manifest.value()));
    }

    void VerifyFile::operator()(Execution::Context& context) const
    {
        std::filesystem::path path = context.Args.GetArg(m_arg);

        if (!std::filesystem::exists(path))
        {
            context.Reporter.Error() << "File does not exist: " << path.u8string() << std::endl;
            AICLI_TERMINATE_CONTEXT(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        }

        if (std::filesystem::is_directory(path))
        {
            context.Reporter.Error() << "Path is a directory: " << path.u8string() << std::endl;
            AICLI_TERMINATE_CONTEXT(HRESULT_FROM_WIN32(ERROR_DIRECTORY_NOT_SUPPORTED));
        }
    }

    void GetManifestFromArg(Execution::Context& context)
    {
        context <<
            VerifyFile(Execution::Args::Type::Manifest) <<
            [](Execution::Context& context)
        {
            Manifest::Manifest manifest = Manifest::Manifest::CreateFromPath(context.Args.GetArg(Execution::Args::Type::Manifest));
            Logging::Telemetry().LogManifestFields(manifest.Id, manifest.Name, manifest.Version);
            context.Add<Execution::Data::Manifest>(std::move(manifest));
        };
    }

    void GetManifest(Execution::Context& context)
    {
        if (context.Args.Contains(Execution::Args::Type::Manifest))
        {
            context << GetManifestFromArg;
        }
        else
        {
            context <<
                OpenSource <<
                SearchSource <<
                EnsureOneMatchFromSearchResult <<
                GetManifestFromSearchResult;
        }
    }

    void SelectInstaller(Execution::Context& context)
    {
        ManifestComparator manifestComparator(context.Args);
        context.Add<Execution::Data::Installer>(manifestComparator.GetPreferredInstaller(context.Get<Execution::Data::Manifest>()));
    }
}

AppInstaller::CLI::Execution::Context& operator<<(AppInstaller::CLI::Execution::Context& context, AppInstaller::CLI::Workflow::WorkflowTask::Func f)
{
    return (context << AppInstaller::CLI::Workflow::WorkflowTask(f));
}

AppInstaller::CLI::Execution::Context& operator<<(AppInstaller::CLI::Execution::Context& context, const AppInstaller::CLI::Workflow::WorkflowTask& task)
{
    if (!context.IsTerminated())
    {
#ifndef AICLI_DISABLE_TEST_HOOKS
        if (context.ShouldExecuteWorkflowTask(task))
#endif
        {
            task(context);
        }
    }
    return context;
}