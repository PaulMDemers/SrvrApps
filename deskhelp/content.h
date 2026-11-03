#ifndef DESKHELP_CONTENT_H
#define DESKHELP_CONTENT_H
#include <srvros/desk_document.h>
struct help_topic { unsigned id;const char *alias,*title,*detail;unsigned application;const struct desk_document_block *blocks;unsigned count; };
extern const struct help_topic help_topics[];
extern const unsigned help_topic_count;
const struct help_topic *help_topic(unsigned id);
const struct help_topic *help_topic_alias(const char *alias);
#endif
