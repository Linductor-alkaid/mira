#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/sqlite_social_directory.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

int scoped_identity_and_revision_resolution() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey team{fixture.principal.tenant_id, Id128::generate()};
    const SocialEnvironmentKey project{team.tenant, Id128::generate()};
    const SocialEnvironmentKey foreign{TenantId::generate(), team.environment};
    const SocialObjectKey first{team, Id128::generate()};
    const SocialObjectKey second{team, Id128::generate()};
    const SocialObjectKey same_object_other_environment{project, first.object};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "social.db";
    options.max_objects = 3;
    auto opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto directory = std::move(opened).value();
    const auto with = [&](const SocialObjectKey &key, const SocialObjectIdentity &identity) {
        return fixture.executor.submit_auto([&] { return directory->activate(key, identity); })
            .get();
    };

    MIRA_CHECK(!with(first, {0, {"Mira"}}));
    MIRA_CHECK(!with(first, {1, {"Mira", " mira "}}));
    MIRA_CHECK(!with(first, {1, {default_forbidden_content_markers().front()}}));
    MIRA_CHECK(with(first, {1, {" 陈工 ", "Alicia   Chen"}}));
    MIRA_CHECK(with(first, {1, {"Alicia   Chen", " 陈工 "}}));
    const auto conflict = with(first, {1, {"different"}});
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(with(second, {3, {"陈工"}}));
    MIRA_CHECK(with(same_object_other_environment, {1, {"陈工"}}));

    const auto ambiguous = directory->resolve(team, " 陈工", 1);
    MIRA_CHECK(ambiguous && ambiguous.value().resolution == SocialAliasResolution::Ambiguous &&
               ambiguous.value().truncated && ambiguous.value().matches.size() == 1);
    const auto all = directory->resolve(team, "陈工");
    MIRA_CHECK(all && all.value().matches.size() == 2 && !all.value().truncated);
    const auto local = directory->resolve(project, "陈工");
    MIRA_CHECK(local && local.value().resolution == SocialAliasResolution::Unique &&
               local.value().matches.front().key == same_object_other_environment);
    MIRA_CHECK(directory->resolve(foreign, "陈工").value().matches.empty());
    MIRA_CHECK(!directory->read({foreign, first.object}).value());
    MIRA_CHECK(directory->resolve(team, " alICia\t cHen ").value().matches.front().key == first);
    MIRA_CHECK(directory->resolve(team, "陈").value().resolution == SocialAliasResolution::Missing);
    MIRA_CHECK(!directory->resolve(team, "陈工", 0));

    const auto over_capacity = with({foreign, Id128::generate()}, {1, {"another"}});
    MIRA_CHECK(!over_capacity && over_capacity.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(with(first, {2, {"A. Chen"}}));
    const auto stale = with(first, {1, {"陈工"}});
    MIRA_CHECK(!stale && stale.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(directory->resolve(team, "陈工").value().matches.front().key == second);
    MIRA_CHECK(directory->close());
    MIRA_CHECK(!directory->read(first));
    directory.reset();

    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    const auto current = directory->read(first);
    MIRA_CHECK(current && current.value() && current.value()->revision == 2);
    MIRA_CHECK(current.value()->aliases == std::vector<std::string>{"A. Chen"});
    MIRA_CHECK(directory->resolve(team, "a. CHEN").value().matches.front().revision == 2);

    MIRA_CHECK(directory->erase_object(first) && directory->erase_object(first));
    MIRA_CHECK(directory->resolve(team, "A. Chen").value().matches.empty());
    MIRA_CHECK(!directory->read(first).value());
    MIRA_CHECK(!with(first, {3, {"replayed"}}));
    MIRA_CHECK(directory->erase_environment(project) && directory->erase_environment(project));
    MIRA_CHECK(directory->resolve(project, "陈工").value().matches.empty());
    MIRA_CHECK(directory->resolve(team, "陈工").value().matches.front().key == second);
    MIRA_CHECK(directory->close());
    directory.reset();

    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    const auto erased_object = with(first, {4, {"replayed object"}});
    MIRA_CHECK(!erased_object && erased_object.error().code == ErrorCode::PermissionDenied);
    const auto erased_environment = with(same_object_other_environment, {2, {"replayed env"}});
    MIRA_CHECK(!erased_environment &&
               erased_environment.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(directory->resolve(project, "陈工").value().matches.empty());
    MIRA_CHECK(directory->resolve(team, "陈工").value().matches.front().key == second);
    return 0;
}

int failed_erasure_fences_until_host_retry() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey team{fixture.principal.tenant_id, Id128::generate()};
    const SocialObjectKey existing{team, Id128::generate()};
    const SocialObjectKey unseen{team, Id128::generate()};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "social.db";
    options.max_objects = 1;
    options.max_environments = 1;
    auto opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto directory = std::move(opened).value();
    MIRA_CHECK(directory->activate(existing, {1, {"owner"}}));
    const auto failed = directory->erase_object(unseen);
    MIRA_CHECK(!failed && failed.error().code == ErrorCode::ResourceExhausted);
    const auto fenced = directory->resolve(team, "owner");
    MIRA_CHECK(!fenced && fenced.error().code == ErrorCode::Unavailable);
    MIRA_CHECK(!directory->activate(existing, {2, {"new owner"}}));
    MIRA_CHECK(directory->close());
    directory.reset();

    options.max_objects = 2;
    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    MIRA_CHECK(directory->erase_object(unseen));
    MIRA_CHECK(!directory->activate(unseen, {1, {"late candidate"}}));
    MIRA_CHECK(directory->resolve(team, "owner").value().matches.front().key == existing);
    MIRA_CHECK(directory->erase_environment(team));
    MIRA_CHECK(directory->resolve(team, "owner").value().matches.empty());
    return 0;
}
} // namespace

int main() {
    if (const auto result = scoped_identity_and_revision_resolution(); result != 0)
        return result;
    return failed_erasure_fences_until_host_retry();
}
