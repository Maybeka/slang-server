//------------------------------------------------------------------------------
// ServerDiagClient.h
// Diagnostic client for the LSP server.
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#pragma once

#include "SlangLspClient.h"
#include "lsp/URI.h"
#include <functional>
#include <utility>

#include "slang/diagnostics/DiagnosticClient.h"
namespace server {
class ServerDiagClient : public slang::DiagnosticClient {
public:
    // Roll back diagnostics from an analysis abandoned by cancellation. Only URIs
    // touched during the update are copied; unrelated document diagnostics stay put.
    class Update {
    public:
        explicit Update(ServerDiagClient& client);
        ~Update();
        Update(const Update&) = delete;
        Update& operator=(const Update&) = delete;
        void commit();

    private:
        friend class ServerDiagClient;
        struct Previous {
            std::optional<std::vector<lsp::Diagnostic>> diagnostics;
            std::optional<int> version;
            bool dirty;
        };
        void remember(const URI& uri);
        ServerDiagClient& client;
        std::unordered_map<URI, Previous> previous;
        bool committed = false;
    };

    Update beginUpdate() { return Update(*this); }

    // Compiler paths identify physical sources; editor URIs identify publications.
    // Bind only known documents, without changing URI equality or source identity.
    void setUriResolver(std::function<URI(const URI&)> resolver) {
        m_uriResolver = std::move(resolver);
    }
    // A source may have published incidental diagnostics before an editor opened
    // it under an alias. Retire that old key only if it actually holds client errors.
    void adoptDocumentUri(const URI& physical, const URI& opened);

    void setVersion(const URI& uri, std::optional<int> version) {
        if (version)
            m_versions.insert_or_assign(uri, *version);
        else
            m_versions.erase(uri);
    }

    ServerDiagClient(const slang::SourceManager& sourceManager, SlangLspClient& client,
                     bool deltaPublishing = false) :
        m_deltaPublishing(deltaPublishing), m_sourceManager(sourceManager), m_client(client) {
        m_diagnostics = std::unordered_map<URI, std::vector<lsp::Diagnostic>>();
    }

    ~ServerDiagClient() {
        if (m_publishOnDestruction)
            clearAndPush();
    }
    void disableDestructorPublication() { m_publishOnDestruction = false; }
    bool publishesOnDestruction() const { return m_publishOnDestruction; }
    // Retiring compiler state does not retire the editor's diagnostic snapshot.
    bool inheritPublications(const ServerDiagClient& previous, bool staged = false) {
        if (!staged && (!m_deltaPublishing || !previous.m_deltaPublishing))
            return false;
        if (staged) {
            // Retain closed-file results too, including when delta publishing is off.
            // A candidate cancelled before commit must never clear these editor results.
            m_diagnostics = previous.m_diagnostics;
            m_versions = previous.m_versions;
        }
        m_publications = previous.m_publications;
        m_publishOnDestruction = false;
        return true;
    }
    void completePublicationHandoff(ServerDiagClient& previous) {
        previous.disableDestructorPublication();
        m_publishOnDestruction = true;
    }
    bool sharesPublicationsWith(const ServerDiagClient& previous) const {
        return m_publications == previous.m_publications;
    }
    const auto& renderedDiagnostics() const { return m_diagnostics; }
    // Import only after the owner has validated the complete snapshot generation.
    void replaceRendered(URI uri, std::vector<lsp::Diagnostic> diagnostics) {
        uri = resolveUri(uri);
        if (m_update)
            m_update->remember(uri);
        m_diagnostics.insert_or_assign(uri, std::move(diagnostics));
        m_dirtyUris.emplace(uri);
    }
    // Scoped observers see physical keys only for diagnostics added to an LSP bucket.
    // Returning the previous observer allows exception-safe restoration by the caller.
    using ReportObserver = std::function<void(const URI&)>;
    ReportObserver exchangeReportObserver(ReportObserver observer) {
        return std::exchange(m_reportObserver, std::move(observer));
    }
    URI resolvePublicationUri(const URI& physical) const { return resolveUri(physical); }
    /// reports from the diagnostic engine
    void report(const slang::ReportedDiagnostic& to_report) override;

    /// report unpublished diags to the client
    void pushDiags();

    /// report unpublished diags to the client, doing the specified uri first
    void pushDiags(const URI& priorityUri);

    // Clear a specific URI's diagnostics, put not publishing to client, since they are still likely
    // relevant
    void clear(URI uri);

    // Clear all diagnostics by publishing empty lists, then clear internal data structures
    // We want to see these go away for compilations, because they may be stale
    void clearAndPush();

    // Clear internal diagnostic data structures without notifying the client
    // Add all uris to dirty so they get republished after new diags are added.
    void clear();

private:
    struct Published {
        std::optional<int> version;
        std::vector<lsp::Diagnostic> diagnostics;
        std::unordered_multimap<size_t, size_t> index;
    };
    ReportObserver m_reportObserver;
    bool m_deltaPublishing;
    struct Publications {
        std::unordered_map<URI, Published> published;
        slang::flat_hash_set<URI> nonempty;
    };
    std::shared_ptr<Publications> m_publications = std::make_shared<Publications>();
    void publish(const URI& uri, const std::vector<lsp::Diagnostic>& diagnostics);
    void pushClearedDiags();
    URI resolveUri(const URI& uri) const { return m_uriResolver ? m_uriResolver(uri) : uri; }
    std::function<URI(const URI&)> m_uriResolver;
    std::optional<int> getVersion(const URI& uri) const {
        auto it = m_versions.find(uri);
        return it == m_versions.end() ? std::nullopt : std::optional{it->second};
    }
    std::unordered_map<URI, int> m_versions;
    bool m_publishOnDestruction = true;
    Update* m_update = nullptr;
    std::unordered_map<URI, std::vector<lsp::Diagnostic>> m_diagnostics;
    // Uris that have modified diags yet to be pushed to the client
    slang::flat_hash_set<URI> m_dirtyUris;

    const slang::SourceManager& m_sourceManager;
    SlangLspClient& m_client;
};
} // namespace server
