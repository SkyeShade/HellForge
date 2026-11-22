/* HfAttachment.set (SDK 25, attachments.c): an attachment's own values, checked as the named fields of deferred edits.
 * Included by fields.c. */

/* An attachment's own values (attachments.c, HfAttachment.set) as delta writes: 0 with *count of them, 1 refused
 * (logged). Any single-value field the loader reviewed goes: the delta writes it on whichever of the weapon's records
 * it names. */
static int AttachmentSets(HfMod* mod, const char* what, const HfSet* set, AttWrite* out, uint32_t* count)
{
    *count = 0;
    for (; set != NULL && set->field.component != NULL; set++)
    {
        const SdkComponent* c = NULL;
        const SdkField* f = ResolveField(mod, what, &set->field, &c);
        if (f == NULL) return 1;
        if (*count == HF_ATTACHMENT_SETS) { ApiLog(mod, "%s: at most %d values", what, HF_ATTACHMENT_SETS); return 1; }
        AttWrite* w = &out[*count];
        if (ValueBytes(mod, what, set->field.line, c, f, &set->value, w->bytes)) return 1;
        w->component = (uint32_t)c->index; w->offset = f->offset; w->size = f->size;
        (*count)++;
    }
    return 0;
}
