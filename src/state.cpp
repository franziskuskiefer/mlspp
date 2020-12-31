#include <mls/state.h>

namespace mls {

///
/// Constructors
///

State::State(bytes group_id,
             CipherSuite suite,
             const HPKEPrivateKey& init_priv,
             SignaturePrivateKey sig_priv,
             const KeyPackage& key_package)
  : _suite(suite)
  , _group_id(std::move(group_id))
  , _epoch(0)
  , _tree(suite)
  , _transcript_hash(suite)
  , _index(0)
  , _identity_priv(std::move(sig_priv))
{
  auto index = _tree.add_leaf(key_package);
  _tree.set_hash_all();
  _tree_priv = TreeKEMPrivateKey::solo(suite, index, init_priv);

  // XXX(RLB): Convert KeyScheduleEpoch to take GroupContext?
  auto ctx = tls::marshal(group_context());
  _key_schedule =
    KeyScheduleEpoch(_suite, random_bytes(_suite.secret_size()), ctx);
  _keys = _key_schedule.encryption_keys(_tree.size());
}

// Initialize a group from a Welcome
State::State(const HPKEPrivateKey& init_priv,
             SignaturePrivateKey sig_priv,
             const KeyPackage& kp,
             const Welcome& welcome)
  : _suite(welcome.cipher_suite)
  , _tree(welcome.cipher_suite)
  , _transcript_hash(welcome.cipher_suite)
  , _identity_priv(std::move(sig_priv))
{
  auto maybe_kpi = welcome.find(kp);
  if (!maybe_kpi) {
    throw InvalidParameterError("Welcome not intended for key package");
  }
  auto kpi = opt::get(maybe_kpi);

  if (kp.cipher_suite != welcome.cipher_suite) {
    throw InvalidParameterError("Ciphersuite mismatch");
  }

  // Decrypt the GroupSecrets
  auto secrets_ct = welcome.secrets[kpi].encrypted_group_secrets;
  auto secrets_data = init_priv.decrypt(kp.cipher_suite, {}, secrets_ct);
  auto secrets = tls::get<GroupSecrets>(secrets_data);

  // Decrypt the GroupInfo
  auto group_info = welcome.decrypt(secrets.joiner_secret, {});
  auto maybe_tree_extn = group_info.extensions.find<RatchetTreeExtension>();
  if (!maybe_tree_extn) {
    throw InvalidParameterError("Ratchet tree not provided in GroupInfo");
  }

  auto& group_info_tree = opt::get(maybe_tree_extn).tree;
  group_info_tree.suite = _suite;
  group_info_tree.set_hash_all();

  // Verify the signature on the GroupInfo
  if (!group_info.verify(group_info_tree)) {
    throw InvalidParameterError("Invalid GroupInfo");
  }

  // Ingest the GroupSecrets and GroupInfo
  _epoch = group_info.epoch;
  _group_id = group_info.group_id;

  _tree = group_info_tree;
  if (!_tree.parent_hash_valid()) {
    throw InvalidParameterError("Invalid tree");
  }

  _transcript_hash.confirmed = group_info.confirmed_transcript_hash;
  _transcript_hash.update_interim(group_info.confirmation_tag);

  // Construct TreeKEM private key from partrs provided
  auto maybe_index = _tree.find(kp);
  if (!maybe_index) {
    throw InvalidParameterError("New joiner not in tree");
  }

  _index = opt::get(maybe_index);

  auto ancestor = tree_math::ancestor(_index, group_info.signer_index);
  auto path_secret = std::optional<bytes>{};
  if (secrets.path_secret) {
    path_secret = opt::get(secrets.path_secret).secret;
  }

  _tree_priv = TreeKEMPrivateKey::joiner(
    _suite, _tree.size(), _index, init_priv, ancestor, path_secret);

  // Ratchet forward into the current epoch
  auto group_ctx = tls::marshal(group_context());
  _key_schedule =
    KeyScheduleEpoch(_suite, secrets.joiner_secret, _suite.zero(), group_ctx);
  _keys = _key_schedule.encryption_keys(_tree.size());

  // Verify the confirmation
  if (!verify_confirmation(group_info.confirmation_tag.mac_value)) {
    throw ProtocolError("Confirmation failed to verify");
  }
}

///
/// Proposal and commit factories
///

MLSPlaintext
State::sign(const Proposal& proposal) const
{
  auto sender = Sender{ SenderType::member, _index.val };
  auto pt = MLSPlaintext{ _group_id, _epoch, sender, proposal };
  pt.sign(_suite, group_context(), _identity_priv);
  pt.membership_tag = { _key_schedule.membership_tag(group_context(), pt) };
  return pt;
}

MLSPlaintext
State::add(const KeyPackage& key_package) const
{
  // Check that the key package is validly signed
  if (!key_package.verify()) {
    throw InvalidParameterError("Invalid signature on key package");
  }

  // Check that the group's basic properties are supported
  auto now = seconds_since_epoch();
  if (!key_package.verify_expiry(now)) {
    throw InvalidParameterError("Expired key package");
  }

  // Check that the group's extensions are supported
  if (!key_package.verify_extension_support(_extensions)) {
    throw InvalidParameterError(
      "Key package does not support group's extensions");
  }

  return sign({ Add{ key_package } });
}

MLSPlaintext
State::update(const bytes& leaf_secret)
{
  // TODO(RLB) Allow changing the signing key
  auto kp = opt::get(_tree.key_package(_index));
  kp.init_key = HPKEPrivateKey::derive(_suite, leaf_secret).public_key;
  kp.sign(_identity_priv, std::nullopt);

  auto pt = sign({ Update{ kp } });

  auto id = proposal_id(pt);
  _update_secrets[id.id] = leaf_secret;

  return pt;
}

LeafIndex
State::leaf_for_roster_entry(RosterIndex index) const
{
  auto non_blank_leaves = uint32_t(0);

  for (auto i = LeafIndex{ 0 }; i < _tree.size(); i.val++) {
    const auto& kp = _tree.key_package(i);
    if (!kp) {
      continue;
    }
    if (non_blank_leaves == index.val) {
      return i;
    }
    non_blank_leaves += 1;
  }

  throw InvalidParameterError("Leaf Index mismatch");
}

MLSPlaintext
State::remove(RosterIndex index) const
{
  return remove(leaf_for_roster_entry(index));
}

MLSPlaintext
State::remove(LeafIndex removed) const
{
  return sign({ Remove{ removed } });
}

std::tuple<MLSPlaintext, Welcome, State>
State::commit(const bytes& leaf_secret) const
{
  // Construct a commit from cached proposals
  // TODO(rlb) ignore some proposals:
  // * Update after Update
  // * Update after Remove
  // * Remove after Remove
  Commit commit;
  auto joiners = std::vector<KeyPackage>{};
  for (const auto& pt : _pending_proposals) {
    auto id = proposal_id(pt);
    const auto& proposal = var::get<Proposal>(pt.content).content;
    if (var::holds_alternative<Add>(proposal)) {
      const auto& add = var::get<Add>(proposal);
      joiners.push_back(add.key_package);
    }

    commit.proposals.push_back(id);
  }

  // Apply proposals
  State next = *this;
  auto [has_updates, has_removes, joiner_locations] = next.apply(commit);
  next._pending_proposals.clear();

  // KEM new entropy to the group and the new joiners
  auto path_required = has_updates || has_removes || commit.proposals.empty();
  auto update_secret = _suite.zero();
  auto path_secrets =
    std::vector<std::optional<bytes>>(joiner_locations.size());
  if (path_required) {
    auto ctx = tls::marshal(GroupContext{
      next._group_id,
      next._epoch + 1,
      next._tree.root_hash(),
      next._transcript_hash.confirmed,
      next._extensions,
    });
    auto [new_priv, path] =
      next._tree.encap(_index, ctx, leaf_secret, _identity_priv, std::nullopt);
    next._tree_priv = new_priv;
    commit.path = path;
    update_secret = new_priv.update_secret;

    for (size_t i = 0; i < joiner_locations.size(); i++) {
      auto [overlap, shared_path_secret, ok] =
        new_priv.shared_path_secret(joiner_locations[i]);
      silence_unused(overlap);
      silence_unused(ok);

      path_secrets[i] = shared_path_secret;
    }
  }

  // Create the Commit message and advance the transcripts / key schedule
  auto pt = next.ratchet_and_sign(commit, update_secret, group_context());

  // Complete the GroupInfo and form the Welcome
  auto group_info = GroupInfo{
    next._group_id,
    next._epoch,
    next._tree.root_hash(),
    next._transcript_hash.confirmed,
    next._extensions,
    opt::get(pt.confirmation_tag),
  };
  group_info.extensions.add(RatchetTreeExtension{ next._tree });
  group_info.sign(next._tree, _index, _identity_priv);

  auto welcome =
    Welcome{ _suite, next._key_schedule.joiner_secret, {}, group_info };
  for (size_t i = 0; i < joiners.size(); i++) {
    welcome.encrypt(joiners[i], path_secrets[i]);
  }

  return std::make_tuple(pt, welcome, next);
}

///
/// Message handlers
///

GroupContext
State::group_context() const
{
  return GroupContext{
    _group_id,   _epoch, _tree.root_hash(), _transcript_hash.confirmed,
    _extensions,
  };
}

MLSPlaintext
State::ratchet_and_sign(const Commit& op,
                        const bytes& update_secret,
                        const GroupContext& prev_ctx)
{
  auto prev_key_schedule = _key_schedule;

  auto sender = Sender{ SenderType::member, _index.val };
  auto pt = MLSPlaintext{ _group_id, _epoch, sender, op };
  pt.sign(_suite, prev_ctx, _identity_priv);

  _transcript_hash.update_confirmed(pt);
  _epoch += 1;
  update_epoch_secrets(update_secret);

  pt.confirmation_tag = { _key_schedule.confirmation_tag(
    _transcript_hash.confirmed) };
  pt.membership_tag = { prev_key_schedule.membership_tag(prev_ctx, pt) };

  _transcript_hash.update_interim(pt);

  return pt;
}

std::optional<State>
State::handle(const MLSPlaintext& pt)
{
  // Pre-validate the MLSPlaintext
  if (pt.group_id != _group_id) {
    throw InvalidParameterError("GroupID mismatch");
  }

  if (pt.epoch != _epoch) {
    throw InvalidParameterError("Epoch mismatch");
  }

  if (!verify(pt)) {
    throw ProtocolError("Invalid handshake message signature");
  }

  // Proposals get queued, do not result in a state transition
  if (var::holds_alternative<Proposal>(pt.content)) {
    _pending_proposals.push_back(pt);
    return std::nullopt;
  }

  if (!var::holds_alternative<Commit>(pt.content)) {
    throw InvalidParameterError("Incorrect content type");
  }

  if (pt.sender.sender_type != SenderType::member) {
    throw ProtocolError("Commit must originate from within the group");
  }
  auto sender = LeafIndex(pt.sender.sender);

  if (sender == _index) {
    throw InvalidParameterError("Handle own commits with caching");
  }

  // Apply the commit
  const auto& commit = var::get<Commit>(pt.content);
  State next = *this;
  next.apply(commit);

  // Decapsulate and apply the UpdatePath, if provided
  auto update_secret = _suite.zero();
  if (commit.path) {
    const auto& path = opt::get(commit.path);
    if (!path.parent_hash_valid(_suite)) {
      throw ProtocolError("Commit path has invalid parent hash");
    }

    auto ctx = tls::marshal(GroupContext{
      next._group_id,
      next._epoch + 1,
      next._tree.root_hash(),
      next._transcript_hash.confirmed,
      next._extensions,
    });
    next._tree_priv.decap(sender, next._tree, ctx, path);
    next._tree.merge(sender, path);
    update_secret = next._tree_priv.update_secret;
  }

  // Update the transcripts and advance the key schedule
  next._transcript_hash.update(pt);
  next._epoch += 1;
  next.update_epoch_secrets(update_secret);

  // Verify the confirmation MAC
  if (!pt.confirmation_tag) {
    throw ProtocolError("Missing confirmation on Commit");
  }

  if (!next.verify_confirmation(opt::get(pt.confirmation_tag).mac_value)) {
    throw ProtocolError("Confirmation failed to verify");
  }

  return next;
}

LeafIndex
State::apply(const Add& add)
{
  return _tree.add_leaf(add.key_package);
}

void
State::apply(LeafIndex target, const Update& update)
{
  _tree.update_leaf(target, update.key_package);
}

void
State::apply(LeafIndex target, const Update& update, const bytes& leaf_secret)
{
  _tree.update_leaf(target, update.key_package);
  _tree_priv.set_leaf_secret(leaf_secret);
}

void
State::apply(const Remove& remove)
{
  _tree.blank_path(remove.removed);
}

ProposalID
State::proposal_id(const MLSPlaintext& pt) const
{
  return ProposalID{ _suite.digest().hash(pt.commit_content()) };
}

std::optional<MLSPlaintext>
State::find_proposal(const ProposalID& id)
{
  for (auto i = _pending_proposals.begin(); i != _pending_proposals.end();
       i++) {
    auto other_id = proposal_id(*i);
    if (id == other_id) {
      auto pt = *i;
      _pending_proposals.erase(i);
      return pt;
    }
  }

  return std::nullopt;
}

std::vector<LeafIndex>
State::apply(const std::vector<MLSPlaintext>& pts, ProposalType required_type)
{
  auto locations = std::vector<LeafIndex>{};
  for (const auto& pt : pts) {
    auto proposal = var::get<Proposal>(pt.content).content;
    auto proposal_type = var::get<Proposal>(pt.content).proposal_type();
    if (proposal_type != required_type) {
      continue;
    }

    switch (proposal_type) {
      case ProposalType::add: {
        locations.push_back(apply(var::get<Add>(proposal)));
        break;
      }

      case ProposalType::update: {
        auto& update = var::get<Update>(proposal);
        auto sender = LeafIndex(pt.sender.sender);
        if (sender != _index) {
          apply(sender, update);
          break;
        }

        auto id = proposal_id(pt);
        if (_update_secrets.count(id.id) == 0) {
          throw ProtocolError("Self-update with no cached secret");
        }

        apply(sender, update, _update_secrets[id.id]);
        locations.push_back(sender);
        break;
      }

      case ProposalType::remove: {
        const auto& remove = var::get<Remove>(proposal);
        apply(remove);
        locations.push_back(remove.removed);
        break;
      }

      default:
        throw ProtocolError("Unknown proposal type");
    }
  }

  return locations;
}

std::tuple<bool, bool, std::vector<LeafIndex>>
State::apply(const Commit& commit)
{
  auto pts = std::vector<MLSPlaintext>(commit.proposals.size());
  std::transform(commit.proposals.begin(),
                 commit.proposals.end(),
                 pts.begin(),
                 [&](auto& id) {
                   auto maybe_pt = find_proposal(id);
                   if (!maybe_pt) {
                     throw ProtocolError("Commit of unknown proposal");
                   }

                   return opt::get(maybe_pt);
                 });

  auto update_locations = apply(pts, ProposalType::update);
  auto remove_locations = apply(pts, ProposalType::remove);
  auto joiner_locations = apply(pts, ProposalType::add);

  auto has_updates = !update_locations.empty();
  auto has_removes = !remove_locations.empty();

  _tree.truncate();
  _tree_priv.truncate(_tree.size());
  _tree.set_hash_all();
  return std::make_tuple(has_updates, has_removes, joiner_locations);
}

///
/// Message protection
///

MLSCiphertext
State::protect(const bytes& pt)
{
  auto sender = Sender{ SenderType::member, _index.val };
  MLSPlaintext mpt{ _group_id, _epoch, sender, ApplicationData{ pt } };
  mpt.sign(_suite, group_context(), _identity_priv);
  mpt.membership_tag = { _key_schedule.membership_tag(group_context(), mpt) };
  return encrypt(mpt);
}

bytes
State::unprotect(const MLSCiphertext& ct)
{
  MLSPlaintext pt = decrypt(ct);

  if (!verify(pt)) {
    throw ProtocolError("Invalid message signature");
  }

  if (!var::holds_alternative<ApplicationData>(pt.content)) {
    throw ProtocolError("Unprotect of non-application message");
  }

  // NOLINTNEXTLINE(cppcoreguidelines-slicing)
  return var::get<ApplicationData>(pt.content).data;
}

///
/// Inner logic and convenience functions
///

bool
operator==(const State& lhs, const State& rhs)
{
  auto suite = (lhs._suite == rhs._suite);
  auto group_id = (lhs._group_id == rhs._group_id);
  auto epoch = (lhs._epoch == rhs._epoch);
  auto tree = (lhs._tree == rhs._tree);
  auto transcript_hash = (lhs._transcript_hash == rhs._transcript_hash);
  auto key_schedule = (lhs._key_schedule == rhs._key_schedule);

  return suite && group_id && epoch && tree && transcript_hash && key_schedule;
}

bool
operator!=(const State& lhs, const State& rhs)
{
  return !(lhs == rhs);
}

void
State::update_epoch_secrets(const bytes& commit_secret)
{
  auto ctx = tls::marshal(GroupContext{
    _group_id,
    _epoch,
    _tree.root_hash(),
    _transcript_hash.confirmed,
    _extensions,
  });
  _key_schedule = _key_schedule.next(commit_secret, _suite.zero(), ctx);
  _keys = _key_schedule.encryption_keys(_tree.size());
}

///
/// Message encryption and decryption
///

bool
State::verify(const MLSPlaintext& pt) const
{
  if (pt.sender.sender_type != SenderType::member) {
    // TODO(RLB) Support external senders
    throw InvalidParameterError("External senders not supported");
  }

  auto membership_tag = _key_schedule.membership_tag(group_context(), pt);
  if (!pt.verify_membership_tag(membership_tag)) {
    return false;
  }

  auto maybe_kp = _tree.key_package(LeafIndex(pt.sender.sender));
  if (!maybe_kp) {
    throw InvalidParameterError("Signature from blank node");
  }

  auto pub = opt::get(maybe_kp).credential.public_key();
  return pt.verify(_suite, group_context(), pub);
}

bool
State::verify_confirmation(const bytes& confirmation) const
{
  auto confirm = _key_schedule.confirmation_tag(_transcript_hash.confirmed);
  return constant_time_eq(confirm, confirmation);
}

bytes
State::do_export(const std::string& label,
                 const bytes& context,
                 size_t size) const
{
  return _key_schedule.do_export(label, context, size);
}

std::vector<KeyPackage>
State::roster() const
{
  auto kps = std::vector<KeyPackage>(_tree.size().val);
  auto leaf_count = uint32_t(0);

  for (uint32_t i = 0; i < _tree.size().val; i++) {
    const auto& kp = _tree.key_package(LeafIndex{ i });
    if (!kp) {
      continue;
    }
    kps.at(leaf_count) = opt::get(kp);
    leaf_count++;
  }

  kps.resize(leaf_count);
  return kps;
}

bytes
State::authentication_secret() const
{
  return _key_schedule.authentication_secret;
}

MLSCiphertext
State::encrypt(const MLSPlaintext& pt)
{
  return _keys.encrypt(_index, _key_schedule.sender_data_secret, pt);
}

MLSPlaintext
State::decrypt(const MLSCiphertext& ct)
{
  // Verify the epoch
  if (ct.group_id != _group_id) {
    throw InvalidParameterError("Ciphertext not from this group");
  }

  if (ct.epoch != _epoch) {
    throw InvalidParameterError("Ciphertext not from this epoch");
  }

  return _keys.decrypt(_key_schedule.sender_data_secret, ct);
}

} // namespace mls
