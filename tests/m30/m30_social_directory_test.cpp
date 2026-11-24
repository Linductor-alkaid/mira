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

int source_accounts_survive_reopen_and_respect_erasure() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey community{fixture.principal.tenant_id, Id128::generate()};
    const SocialEnvironmentKey other_room{community.tenant, Id128::generate()};
    const SocialEnvironmentKey other_tenant{TenantId::generate(), community.environment};
    const SocialAccountKey bilibili{"bilibili", Id128::generate()};
    const SocialAccountKey qq{"qq", bilibili.account};
    const SocialAccountKey other_qq{"qq", Id128::generate()};
    const auto first_message = EventId::generate();
    const auto link_evidence = EventId::generate();
    const auto later_message = EventId::generate();
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "social-accounts.db";
    options.max_accounts = 4;
    auto opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto directory = std::move(opened).value();

    MIRA_CHECK(!directory->resolve_account(community, bilibili).value());
    MIRA_CHECK(!directory->observe_account(community, {"QQ", qq.account}, first_message));
    MIRA_CHECK(!directory->observe_account(community, qq, {}));
    auto observed = fixture.executor.submit_auto(
        [&] { return directory->observe_account(community, bilibili, first_message); });
    const auto first = directory->observe_account(community, bilibili, first_message);
    const auto concurrent = observed.get();
    MIRA_CHECK(first && concurrent && first.value().object == concurrent.value().object);
    const auto person = first.value().object;
    MIRA_CHECK(first.value().source_event == first_message && first.value().object_revision == 1);
    MIRA_CHECK(directory->read(person).value()->aliases.empty());
    MIRA_CHECK(!directory->resolve_account(community, qq).value());
    MIRA_CHECK(!directory->resolve_account(other_tenant, bilibili).value());
    MIRA_CHECK(!directory->observe_account(other_room, qq, link_evidence, person));
    const auto missing = directory->observe_account(community, qq, link_evidence,
                                                    SocialObjectKey{community, Id128::generate()});
    MIRA_CHECK(!missing && missing.error().code == ErrorCode::NotFound);
    MIRA_CHECK(directory->observe_account(community, qq, link_evidence, person).value().object ==
               person);

    const auto other = directory->observe_account(community, other_qq, first_message);
    MIRA_CHECK(other && other.value().object != person);
    MIRA_CHECK(directory->activate(person, {2, {"同名"}}));
    MIRA_CHECK(directory->activate(other.value().object, {2, {"同名"}}));
    MIRA_CHECK(directory->resolve(community, "同名").value().resolution ==
               SocialAliasResolution::Ambiguous);
    MIRA_CHECK(directory->resolve_account(community, qq).value()->object == person);
    MIRA_CHECK(directory->resolve_account(community, other_qq).value()->object ==
               other.value().object);
    const auto conflict = directory->observe_account(community, other_qq, link_evidence, person);
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::AlreadyExists);
    const auto separated = directory->observe_account(other_room, qq, first_message);
    MIRA_CHECK(separated && separated.value().object != person);
    const auto limit = directory->observe_account(other_tenant, qq, first_message);
    MIRA_CHECK(!limit && limit.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(directory->close());
    directory.reset();

    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    const auto replay = directory->observe_account(community, qq, later_message);
    MIRA_CHECK(replay && replay.value().object == person && replay.value().object_revision == 2);
    MIRA_CHECK(replay.value().source_event == link_evidence);
    MIRA_CHECK(directory->erase_object(person));
    const auto erased = directory->resolve_account(community, bilibili);
    MIRA_CHECK(!erased && erased.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(!directory->observe_account(community, qq, later_message));
    MIRA_CHECK(directory->resolve_account(other_room, qq).value()->object ==
               separated.value().object);
    MIRA_CHECK(directory->resolve_account(community, other_qq).value());
    MIRA_CHECK(directory->erase_environment(other_room));
    MIRA_CHECK(!directory->resolve_account(other_room, qq).value());
    MIRA_CHECK(!directory->observe_account(other_room, qq, later_message));
    MIRA_CHECK(directory->close());
    directory.reset();

    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    const auto late = directory->observe_account(community, qq, link_evidence);
    MIRA_CHECK(!late && late.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(directory->observe_account(other_tenant, qq, later_message));
    return 0;
}

int messages_keep_scoped_speaker_evidence() {
    MemoryAgentFixture fixture;
    const auto session = SessionId::generate();
    const SocialEnvironmentKey environment{fixture.principal.tenant_id, Id128::generate()};
    const SocialEnvironmentKey other{environment.tenant, Id128::generate()};
    const SocialAccountKey qq{"qq", Id128::generate()};
    const SocialAccountKey bilibili{"bilibili", Id128::generate()};
    const SocialAccountKey other_speaker{"qq", Id128::generate()};
    const SocialAccountKey fresh{"qq", Id128::generate()};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "social-messages.db";
    options.max_messages = 4;
    auto opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto directory = std::move(opened).value();
    const auto append = [&](IEventStore &store, EventId id,
                            std::string body) -> Result<SocialMessageLocator> {
        AppendRequest event;
        event.event_id = id;
        event.session_id = session;
        event.runtime_id = fixture.runtime;
        event.payload = {"mira.social.message.received.v1", std::move(body), EventClass::State};
        const auto saved = store.append(event);
        if (!saved)
            return saved.error();
        return SocialMessageLocator{session, saved.value().session_sequence,
                                    saved.value().event_id};
    };
    auto &events = *fixture.events;
    const auto first = append(events, EventId::generate(), "a message");
    const auto second = append(events, EventId::generate(), "same speaker, another platform");
    const auto third = append(events, EventId::generate(), "another speaker");
    const auto fourth = append(events, EventId::generate(), "capacity check");
    MIRA_CHECK(first && second && third && fourth);
    auto wrong = first.value();
    wrong.event = EventId::generate();
    const auto mismatch = directory->index_message(environment, qq, wrong, events);
    MIRA_CHECK(!mismatch && mismatch.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(!directory->resolve_account(environment, qq).value());

    const auto observed = directory->index_message(environment, qq, first.value(), events);
    MIRA_CHECK(observed && observed.value().source == "qq");
    const auto person = observed.value().speaker;
    const auto known = directory->resolve_account(environment, qq);
    MIRA_CHECK(known && known.value() && known.value()->source_event == first.value().event);
    MIRA_CHECK(directory->observe_account(environment, bilibili, first.value().event, person));
    const auto cross = directory->index_message(environment, bilibili, second.value(), events);
    MIRA_CHECK(cross && cross.value().speaker == person && cross.value().source == "bilibili");
    MIRA_CHECK(cross.value().account_digest != observed.value().account_digest);
    const auto different =
        directory->index_message(environment, other_speaker, third.value(), events);
    MIRA_CHECK(different && different.value().speaker != person);
    MemoryEventStore occupied;
    const auto same_position =
        append(occupied, EventId::generate(), "another event at position one");
    MIRA_CHECK(same_position);
    const auto conflict =
        directory->index_message(environment, fresh, same_position.value(), occupied);
    MIRA_CHECK(!conflict && conflict.error().code == ErrorCode::AlreadyExists);
    MIRA_CHECK(!directory->resolve_account(environment, fresh).value());
    MIRA_CHECK(directory->read_message_evidence(other, session, 0, 4).value().evidence.empty());
    MIRA_CHECK(directory->read_message_evidence(environment, SessionId::generate(), 0, 4)
                   .value()
                   .evidence.empty());
    MIRA_CHECK(directory->index_message(other, qq, first.value(), events));

    const auto limited = directory->read_message_evidence(environment, session, 0, 3, 2);
    MIRA_CHECK(limited && limited.value().has_more && limited.value().evidence.size() == 2);
    MIRA_CHECK(limited.value().evidence.at(0) == observed.value() &&
               limited.value().evidence.at(1) == cross.value());
    const auto tail = directory->read_message_evidence(environment, session, 2, 3, 2);
    MIRA_CHECK(tail && !tail.value().has_more && tail.value().evidence.size() == 1);
    MIRA_CHECK(tail.value().evidence.front() == different.value());
    const auto over = directory->index_message(environment, fresh, fourth.value(), events);
    MIRA_CHECK(!over && over.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(!directory->resolve_account(environment, fresh).value());

    const auto different_sender =
        directory->index_message(environment, fresh, first.value(), events);
    MIRA_CHECK(!different_sender && different_sender.error().code == ErrorCode::AlreadyExists);
    MemoryEventStore alternate;
    const auto changed = append(alternate, first.value().event, "changed body");
    MIRA_CHECK(changed && changed.value() == first.value());
    const auto different_body =
        directory->index_message(environment, qq, changed.value(), alternate);
    MIRA_CHECK(!different_body && different_body.error().code == ErrorCode::AlreadyExists);
    MIRA_CHECK(directory->close());
    directory.reset();

    opened = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(opened);
    directory = std::move(opened).value();
    fixture.events.reset();
    FileEventStore reopened(fixture.root / "events");
    const auto replay = directory->index_message(environment, qq, first.value(), reopened);
    MIRA_CHECK(replay && replay.value() == observed.value());
    MIRA_CHECK(directory->erase_object(person));
    const auto after_erasure = directory->read_message_evidence(environment, session, 0, 4);
    MIRA_CHECK(after_erasure && after_erasure.value().evidence.size() == 1 &&
               after_erasure.value().evidence.front() == different.value());
    const auto replay_as_other =
        directory->index_message(environment, fresh, first.value(), reopened);
    MIRA_CHECK(!replay_as_other && replay_as_other.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(directory->read_message_evidence(other, session, 0, 4).value().evidence.size() == 1);
    MIRA_CHECK(directory->erase_environment(other));
    MIRA_CHECK(directory->read_message_evidence(other, session, 0, 4).value().evidence.empty());
    MIRA_CHECK(directory->index_message(environment, fresh, fourth.value(), reopened));
    return 0;
}
} // namespace

int main() {
    if (const auto result = scoped_identity_and_revision_resolution(); result != 0)
        return result;
    if (const auto result = failed_erasure_fences_until_host_retry(); result != 0)
        return result;
    if (const auto result = source_accounts_survive_reopen_and_respect_erasure(); result != 0)
        return result;
    return messages_keep_scoped_speaker_evidence();
}
