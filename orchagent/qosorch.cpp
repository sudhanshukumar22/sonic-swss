#include "tokenize.h"
#include "qosorch.h"
#include "logger.h"

#include <sstream>
#include <iostream>

using namespace std;

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_scheduler_api_t *sai_scheduler_api;
extern sai_wred_api_t *sai_wred_api;
extern sai_qos_map_api_t *sai_qos_map_api;
extern sai_scheduler_group_api_t *sai_scheduler_group_api;
extern sai_switch_api_t *sai_switch_api;

extern PortsOrch *gPortsOrch;

map<string, sai_ecn_mark_mode_t> ecn_map = {
    {"ecn_none", SAI_ECN_MARK_MODE_NONE},
    {"ecn_green", SAI_ECN_MARK_MODE_GREEN},
    {"ecn_yellow", SAI_ECN_MARK_MODE_YELLOW},
    {"ecn_red",SAI_ECN_MARK_MODE_RED},
    {"ecn_green_yellow", SAI_ECN_MARK_MODE_GREEN_YELLOW},
    {"ecn_green_red", SAI_ECN_MARK_MODE_GREEN_RED},
    {"ecn_yellow_red", SAI_ECN_MARK_MODE_YELLOW_RED},
    {"ecn_all", SAI_ECN_MARK_MODE_ALL}
};

type_map QosOrch::m_qos_type_maps = {
    {APP_DSCP_TO_TC_MAP_TABLE_NAME, new object_map()},
    {APP_TC_TO_QUEUE_MAP_TABLE_NAME, new object_map()},
    {APP_SCHEDULER_TABLE_NAME, new object_map()},
    {APP_WRED_PROFILE_TABLE_NAME, new object_map()},
    {APP_PORT_QOS_MAP_TABLE_NAME, new object_map()},
    {APP_QUEUE_TABLE_NAME, new object_map()},
    {APP_TC_TO_PRIORITY_GROUP_MAP_NAME, new object_map()},
    {APP_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_NAME, new object_map()},
    {APP_PFC_PRIORITY_TO_QUEUE_MAP_NAME, new object_map()}
};

task_process_status QosMapHandler::processWorkItem(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string qos_object_name = kfvKey(tuple);
    string qos_map_type_name = consumer.m_consumer->getTableName();
    string op = kfvOp(tuple);

    if (QosOrch::getTypeMap()[qos_map_type_name]->find(qos_object_name) != QosOrch::getTypeMap()[qos_map_type_name]->end())
    {
        sai_object = (*(QosOrch::getTypeMap()[qos_map_type_name]))[qos_object_name];
    }
    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> attributes;
        if (!convertFieldValuesToAttributes(tuple, attributes))
        {
            return task_process_status::task_invalid_entry;
        }
        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            if (!modifyQosItem(sai_object, attributes))
            {
                SWSS_LOG_ERROR("Failed to set settings to existing dscp_to_tc map. db name:%s sai object:%llx",
                    qos_object_name.c_str(), sai_object);
                freeAttribResources(attributes);
                return task_process_status::task_failed;
            }
        }
        else {
            sai_object = addQosItem(attributes);
            if (sai_object == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Failed to create dscp_to_tc map. db name:%s", qos_object_name.c_str());
                freeAttribResources(attributes);
                return task_process_status::task_failed;
            }
            (*(QosOrch::getTypeMap()[qos_map_type_name]))[qos_object_name] = sai_object;
        }
        freeAttribResources(attributes);
    }
    else if (op == DEL_COMMAND)
    {
        if (SAI_NULL_OBJECT_ID == sai_object)
        {
            SWSS_LOG_ERROR("Object with name:%s not found.\n", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        if (!removeQosItem(sai_object))
        {
            SWSS_LOG_ERROR("Failed to remove dscp_to_tc map. db name:%s sai object:%llx", qos_object_name.c_str(), sai_object);
            return task_process_status::task_failed;
        }
        auto it_to_delete = (QosOrch::getTypeMap()[qos_map_type_name])->find(qos_object_name);
        (QosOrch::getTypeMap()[qos_map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

bool QosMapHandler::modifyQosItem(sai_object_id_t sai_object, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status = sai_qos_map_api->set_qos_map_attribute(sai_object, &attributes[0]);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to modify map. status:%d", sai_status);
        return false;
    }
    return true;
}

bool QosMapHandler::removeQosItem(sai_object_id_t sai_object)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("Removing QosMap object:%llx", sai_object);
    sai_status_t sai_status = sai_qos_map_api->remove_qos_map(sai_object);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to remove map, status:%d", sai_status);
        return false;
    }
    return true;
}

void QosMapHandler::freeAttribResources(vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    delete[] attributes[0].value.qosmap.list;
}

bool DscpToTcMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t list_attr;
    sai_qos_map_list_t dscp_map_list;
    dscp_map_list.count = kfvFieldsValues(tuple).size();
    dscp_map_list.list = new sai_qos_map_t[dscp_map_list.count];
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        dscp_map_list.list[ind].key.dscp = stoi(fvField(*i));
        dscp_map_list.list[ind].value.tc = stoi(fvValue(*i));
        SWSS_LOG_DEBUG("key.dscp:%d, value.tc:%d", dscp_map_list.list[ind].key.dscp, dscp_map_list.list[ind].value.tc);
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = dscp_map_list.count;
    list_attr.value.qosmap.list = dscp_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t DscpToTcMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t qos_map_attrs[2];
    qos_map_attrs[0].id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attrs[0].value.u32 = SAI_QOS_MAP_DSCP_TO_TC;
    qos_map_attrs[1].id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attrs[1].value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attrs[1].value.qosmap.list = attributes[0].value.qosmap.list;
    sai_status = sai_qos_map_api->create_qos_map(&sai_object, 2, qos_map_attrs);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create dscp_to_tc map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    SWSS_LOG_DEBUG("created QosMap object:%llx", sai_object);
    return sai_object;
}

task_process_status QosOrch::handleDscpToTcTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    DscpToTcMapHandler dscp_tc_handler;
    return dscp_tc_handler.processWorkItem(consumer);
}

bool TcToQueueMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t list_attr;
    sai_qos_map_list_t tc_map_list;
    tc_map_list.count = kfvFieldsValues(tuple).size();
    tc_map_list.list = new sai_qos_map_t[tc_map_list.count];
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        tc_map_list.list[ind].key.tc = stoi(fvField(*i));
        tc_map_list.list[ind].value.queue_index = stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = tc_map_list.count;
    list_attr.value.qosmap.list = tc_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t TcToQueueMapHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t qos_map_attrs[2];
    qos_map_attrs[0].id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attrs[0].value.s32 = SAI_QOS_MAP_TC_TO_QUEUE;
    qos_map_attrs[1].id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attrs[1].value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attrs[1].value.qosmap.list = attributes[0].value.qosmap.list;
    sai_status = sai_qos_map_api->create_qos_map(&sai_object, 2, qos_map_attrs);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;
}

task_process_status QosOrch::handleTcToQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    TcToQueueMapHandler tc_queue_handler;
    return tc_queue_handler.processWorkItem(consumer);
}

void WredMapHandler::freeAttribResources(vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
}

bool WredMapHandler::convertBool(string str, bool &val)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("input:%s", str.c_str());
    if("true" == str)
    {
        val = true;
    }
    else if ("false" == str)
    {
        val = false;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid input specified");
        return false;
    }
    return true;
}

bool WredMapHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == yellow_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);

            // set min threshold to the same value as MAX
            attr.id = SAI_WRED_ATTR_YELLOW_MIN_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == green_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_GREEN_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);

            // set min threshold to the same value as MAX
            attr.id = SAI_WRED_ATTR_GREEN_MIN_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == red_max_threshold_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_MAX_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);

            // set min threshold to the same value as MAX
            attr.id = SAI_WRED_ATTR_RED_MIN_THRESHOLD;
            attr.value.s32 = stoi(fvValue(*i));
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_green_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_GREEN_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_yellow_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_YELLOW_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == wred_red_enable_field_name)
        {
            attr.id = SAI_WRED_ATTR_RED_ENABLE;
            if(!convertBool(fvValue(*i),attr.value.booldata))
            {
                return false;
            }
            attribs.push_back(attr);
        }
        else if (fvField(*i) == ecn_field_name)
        {
            attr.id = SAI_WRED_ATTR_ECN_MARK_MODE;
            sai_ecn_mark_mode_t ecn = ecn_map.at(fvValue(*i));
            attr.value.s32 = ecn;
            attribs.push_back(attr);
        }
        else {
            SWSS_LOG_ERROR("Unkonwn wred profile field:%s", fvField(*i).c_str());
            return false;
        }
    }
    return true;
}

bool WredMapHandler::modifyQosItem(sai_object_id_t sai_object, vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    for (auto attr : attribs)
    {
        sai_status = sai_wred_api->set_wred_attribute(sai_object, &attr);
        if (sai_status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set wred profile attribute, id:%d, status:%d", attr.id, sai_status);
            return false;
        }
    }
    return true;
}

sai_object_id_t WredMapHandler::addQosItem(const vector<sai_attribute_t> &attribs)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_WRED_ATTR_GREEN_DROP_PROBABILITY;
    attr.value.s32 = 100;
    attrs.push_back(attr);

    attr.id = SAI_WRED_ATTR_YELLOW_DROP_PROBABILITY;
    attr.value.s32 = 100;
    attrs.push_back(attr);

    attr.id = SAI_WRED_ATTR_WEIGHT;
    attr.value.s32 = 0;
    attrs.push_back(attr);

    for(auto attrib : attribs)
    {
        attrs.push_back(attrib);
    }
    sai_status = sai_wred_api->create_wred_profile(&sai_object, attrs.size(), attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create wred profile: %d", sai_status);
        return false;
    }
    return sai_object;
}

bool WredMapHandler::removeQosItem(sai_object_id_t sai_object)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_status = sai_wred_api->remove_wred_profile(sai_object);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to remove scheduler profile, status:%d", sai_status);
        return false;
    }
    return true;
}

task_process_status QosOrch::handleWredProfileTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    WredMapHandler wred_handler;
    return wred_handler.processWorkItem(consumer);
}

bool TcToPgHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  tc_to_pg_map_list;
    tc_to_pg_map_list.count = kfvFieldsValues(tuple).size();
    tc_to_pg_map_list.list = new sai_qos_map_t[tc_to_pg_map_list.count];
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        tc_to_pg_map_list.list[ind].key.tc = stoi(fvField(*i));
        tc_to_pg_map_list.list[ind].value.pg = stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = tc_to_pg_map_list.count;
    list_attr.value.qosmap.list = tc_to_pg_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t TcToPgHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t qos_map_attrs[2];
    qos_map_attrs[0].id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attrs[0].value.s32 = SAI_QOS_MAP_TC_TO_PRIORITY_GROUP;
    qos_map_attrs[1].id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attrs[1].value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attrs[1].value.qosmap.list = attributes[0].value.qosmap.list;
    sai_status = sai_qos_map_api->create_qos_map(&sai_object, 2, qos_map_attrs);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

task_process_status QosOrch::handleTcToPgTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    TcToPgHandler tc_to_pg_handler;
    return tc_to_pg_handler.processWorkItem(consumer);
}

bool PfcPrioToPgHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  pfc_prio_to_pg_map_list;
    pfc_prio_to_pg_map_list.count = kfvFieldsValues(tuple).size();
    pfc_prio_to_pg_map_list.list = new sai_qos_map_t[pfc_prio_to_pg_map_list.count];
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        pfc_prio_to_pg_map_list.list[ind].key.prio = stoi(fvField(*i));
        pfc_prio_to_pg_map_list.list[ind].value.pg = stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = pfc_prio_to_pg_map_list.count;
    list_attr.value.qosmap.list = pfc_prio_to_pg_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t PfcPrioToPgHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t qos_map_attrs[2];
    qos_map_attrs[0].id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attrs[0].value.s32 = SAI_QOS_MAP_PFC_PRIORITY_TO_PRIORITY_GROUP;
    qos_map_attrs[1].id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attrs[1].value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attrs[1].value.qosmap.list = attributes[0].value.qosmap.list;
    sai_status = sai_qos_map_api->create_qos_map(&sai_object, 2, qos_map_attrs);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

task_process_status QosOrch::handlePfcPrioToPgTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    PfcPrioToPgHandler pfc_prio_to_pg_handler;
    return pfc_prio_to_pg_handler.processWorkItem(consumer);
}

bool PfcToQueueHandler::convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_attribute_t     list_attr;
    sai_qos_map_list_t  pfc_to_queue_map_list;
    pfc_to_queue_map_list.count = kfvFieldsValues(tuple).size();
    pfc_to_queue_map_list.list = new sai_qos_map_t[pfc_to_queue_map_list.count];
    uint32_t ind = 0;
    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++, ind++)
    {
        pfc_to_queue_map_list.list[ind].key.prio = stoi(fvField(*i));
        pfc_to_queue_map_list.list[ind].value.queue_index = stoi(fvValue(*i));
    }
    list_attr.id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    list_attr.value.qosmap.count = pfc_to_queue_map_list.count;
    list_attr.value.qosmap.list = pfc_to_queue_map_list.list;
    attributes.push_back(list_attr);
    return true;
}

sai_object_id_t PfcToQueueHandler::addQosItem(const vector<sai_attribute_t> &attributes)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object;
    sai_attribute_t qos_map_attrs[2];
    qos_map_attrs[0].id = SAI_QOS_MAP_ATTR_TYPE;
    qos_map_attrs[0].value.s32 = SAI_QOS_MAP_PFC_PRIORITY_TO_QUEUE;
    qos_map_attrs[1].id = SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST;
    qos_map_attrs[1].value.qosmap.count = attributes[0].value.qosmap.count;
    qos_map_attrs[1].value.qosmap.list = attributes[0].value.qosmap.list;
    sai_status = sai_qos_map_api->create_qos_map(&sai_object, 2, qos_map_attrs);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to create tc_to_queue map. status:%d", sai_status);
        return SAI_NULL_OBJECT_ID;
    }
    return sai_object;

}

task_process_status QosOrch::handlePfcToQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    PfcToQueueHandler pfc_to_queue_handler;
    return pfc_to_queue_handler.processWorkItem(consumer);
}

QosOrch::QosOrch(DBConnector *db, vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    initTableHandlers();
};

type_map& QosOrch::getTypeMap()
{
    SWSS_LOG_ENTER();
    return m_qos_type_maps;
}

void QosOrch::initTableHandlers()
{
    SWSS_LOG_ENTER();
    m_qos_handler_map.insert(qos_handler_pair(APP_DSCP_TO_TC_MAP_TABLE_NAME, &QosOrch::handleDscpToTcTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_TC_TO_QUEUE_MAP_TABLE_NAME, &QosOrch::handleTcToQueueTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_SCHEDULER_TABLE_NAME, &QosOrch::handleSchedulerTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_QUEUE_TABLE_NAME, &QosOrch::handleQueueTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_PORT_QOS_MAP_TABLE_NAME, &QosOrch::handlePortQosMapTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_WRED_PROFILE_TABLE_NAME, &QosOrch::handleWredProfileTable));

    m_qos_handler_map.insert(qos_handler_pair(APP_TC_TO_PRIORITY_GROUP_MAP_NAME, &QosOrch::handleTcToPgTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_NAME, &QosOrch::handlePfcPrioToPgTable));
    m_qos_handler_map.insert(qos_handler_pair(APP_PFC_PRIORITY_TO_QUEUE_MAP_NAME, &QosOrch::handlePfcToQueueTable));
}

task_process_status QosOrch::handleSchedulerTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    sai_status_t sai_status;
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    string qos_map_type_name = consumer.m_consumer->getTableName();
    string qos_object_name = kfvKey(tuple);
    string op = kfvOp(tuple);

    if (qos_map_type_name != APP_SCHEDULER_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Key with invalid table type passed in %s, expected:%s\n", qos_map_type_name.c_str(), APP_SCHEDULER_TABLE_NAME);
        return task_process_status::task_invalid_entry;
    }
    if (m_qos_type_maps[qos_map_type_name]->find(qos_object_name) != m_qos_type_maps[qos_map_type_name]->end())
    {
        sai_object = (*(m_qos_type_maps[qos_map_type_name]))[qos_object_name];
        if (sai_object == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Error sai_object must exist for key %s\n", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
    }
    if (op == SET_COMMAND)
    {
        vector<sai_attribute_t> sai_attr_list;
        sai_attribute_t attr;
        for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
        {
            if (fvField(*i) == scheduler_algo_type_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_SCHEDULING_ALGORITHM;
                if (fvValue(*i) == scheduler_algo_DWRR)
                {
                    attr.value.s32 = SAI_SCHEDULING_DWRR;
                }
                else if (fvValue(*i) == scheduler_algo_WRR)
                {
                    attr.value.s32 = SAI_SCHEDULING_WRR;
                }
                else if (fvValue(*i) == scheduler_algo_STRICT)
                {
                    attr.value.s32 = SAI_SCHEDULING_STRICT;
                }
                else {
                    SWSS_LOG_ERROR("Unknown scheduler type value:%s", fvField(*i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_weight_field_name)
            {
                attr.id = SAI_SCHEDULER_ATTR_SCHEDULING_WEIGHT;
                attr.value.u8 = stoi(fvValue(*i));
                sai_attr_list.push_back(attr);
            }
            else if (fvField(*i) == scheduler_priority_field_name)
            {
                // TODO: The meaning is to be able to adjus priority of the given scheduler group.
                // However currently SAI model does not provide such ability.
            }
            else {
                SWSS_LOG_ERROR("Unknown field:%s", fvField(*i).c_str());
                return task_process_status::task_invalid_entry;
            }
        }

        if (SAI_NULL_OBJECT_ID != sai_object)
        {
            for (auto attr : sai_attr_list)
            {
                sai_status = sai_scheduler_api->set_scheduler_attribute(sai_object, &attr);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("fail to set scheduler attribute, id:%d", attr.id);
                    return task_process_status::task_failed;
                }
            }
        }
        else
        {
            sai_status = sai_scheduler_api->create_scheduler_profile(&sai_object, sai_attr_list.size(), sai_attr_list.data());
            if (sai_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("fail to call sai_scheduler_api->create_scheduler_profile: %d", sai_status);
                return task_process_status::task_failed;
            }
            SWSS_LOG_NOTICE("Create scheduler profile");
            (*(m_qos_type_maps[qos_map_type_name]))[qos_object_name] = sai_object;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (SAI_NULL_OBJECT_ID == sai_object)
        {
            SWSS_LOG_ERROR("Object with name:%s not found.\n", qos_object_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        sai_status = sai_scheduler_api->remove_scheduler_profile(sai_object);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to remove scheduler profile. db name:%s sai object:%llx", qos_object_name.c_str(), sai_object);
            return task_process_status::task_failed;
        }
        auto it_to_delete = (m_qos_type_maps[qos_map_type_name])->find(qos_object_name);
        (m_qos_type_maps[qos_map_type_name])->erase(it_to_delete);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        return task_process_status::task_invalid_entry;
    }
    return task_process_status::task_success;
}

bool QosOrch::applySchedulerToQueueSchedulerGroup(Port &port, size_t queue_ind, sai_object_id_t scheduler_profile_id)
{
    SWSS_LOG_ENTER();
    sai_attribute_t            attr;
    sai_status_t               sai_status;
    sai_object_id_t            queue_id;
    vector<sai_object_id_t>    groups;
    vector<sai_object_id_t>    child_groups;
    uint32_t                   groups_count     = 0;

    if (port.m_queue_ids.size() <= queue_ind)
    {
        SWSS_LOG_ERROR("Invalid queue index specified:%d", queue_ind);
        return false;
    }
    queue_id = port.m_queue_ids[queue_ind];

    /* Get max child groups count */
    attr.id = SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_CHILDS_PER_SCHEDULER_GROUP;
    sai_status = sai_switch_api->get_switch_attribute(1, &attr);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to get number of childs per scheduler group for port:%s", port.m_alias.c_str());
        return false;
    }
    child_groups.resize(attr.value.u32);

    /* Get max sched groups count */
    attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_SCHEDULER_GROUPS;
    sai_status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to get number of scheduler groups for port:%s", port.m_alias.c_str());
        return false;
    }

    /* Get total groups list on the port */
    groups_count = attr.value.u32;
    groups.resize(groups_count);

    attr.id = SAI_PORT_ATTR_QOS_SCHEDULER_GROUP_LIST;
    attr.value.objlist.list = groups.data();
    attr.value.objlist.count = groups_count;
    sai_status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (SAI_STATUS_SUCCESS != sai_status)
    {
        SWSS_LOG_ERROR("Failed to get scheduler group list for port:%s", port.m_alias.c_str());
        return false;
    }

    /* Lookup groups to which queue belongs */
    for (uint32_t ii = 0; ii < groups_count ; ii++)
    {
        uint32_t child_count = 0;

        attr.id = SAI_SCHEDULER_GROUP_ATTR_CHILD_COUNT;//Number of queues/groups childs added to scheduler group
        sai_status = sai_scheduler_group_api->get_scheduler_group_attribute(groups[ii], 1, &attr);
        child_count = attr.value.u32;
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to get child count for scheduler group:0x%llx of port:%s", groups[ii], port.m_alias.c_str());
            return false;
        }

        attr.id = SAI_SCHEDULER_GROUP_ATTR_CHILD_LIST;
        attr.value.objlist.list = child_groups.data();
        attr.value.objlist.count = child_count;
        sai_status = sai_scheduler_group_api->get_scheduler_group_attribute(groups[ii], 1, &attr);
        if (SAI_STATUS_SUCCESS != sai_status)
        {
            SWSS_LOG_ERROR("Failed to get child list for scheduler group:0x%llx of port:%s", groups[ii], port.m_alias.c_str());
            return false;
        }

        for (uint32_t jj = 0; jj < child_count; jj++)
        {
            if (child_groups[jj] != queue_id)
            {
                continue;
            }

            attr.id = SAI_SCHEDULER_GROUP_ATTR_SCHEDULER_PROFILE_ID;
            attr.value.oid = scheduler_profile_id;
            sai_status = sai_scheduler_group_api->set_scheduler_group_attribute(groups[ii], &attr);
            if (SAI_STATUS_SUCCESS != sai_status)
            {
                SWSS_LOG_ERROR("Failed applying scheduler profile:0x%llx to scheduler group:0x%llx, port:%s", scheduler_profile_id, groups[ii], port.m_alias.c_str());
                return false;
            }
            SWSS_LOG_DEBUG("port:%s, scheduler_profile_id:0x%llx applied to scheduler group:0x%llx", port.m_alias.c_str(), scheduler_profile_id, groups[ii]);
            return true;
        }
    }
    return false;
}

bool QosOrch::applyWredProfileToQueue(Port &port, size_t queue_ind, sai_object_id_t sai_wred_profile)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t    sai_status;
    sai_object_id_t queue_id;

    if (port.m_queue_ids.size() <= queue_ind)
    {
        SWSS_LOG_ERROR("Invalid queue index specified:%d", queue_ind);
        return false;
    }
    queue_id = port.m_queue_ids[queue_ind];

    attr.id = SAI_QUEUE_ATTR_WRED_PROFILE_ID;
    attr.value.oid = sai_wred_profile;
    sai_status = sai_queue_api->set_queue_attribute(queue_id, &attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set queue attribute:%d", sai_status);
        return false;
    }
    return true;
}

task_process_status QosOrch::handleQueueTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    Port port;
    bool result;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    size_t queue_ind = 0;
    vector<string> tokens;

    sai_uint32_t range_low, range_high;
    vector<string> port_names;

    ref_resolve_status  resolve_result;
    // sample "QUEUE_TABLE:ETHERNET4:1"
    tokens = tokenize(key, delimiter);
    if (tokens.size() != 2)
    {
        SWSS_LOG_ERROR("malformed key:%s. Must contain 2 tokens\n", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    port_names = tokenize(tokens[0], list_item_delimiter);
    if (!parseIndexRange(tokens[1], range_low, range_high))
    {
        SWSS_LOG_ERROR("Failed to parse range:%s", tokens[1].c_str());
        return task_process_status::task_invalid_entry;
    }
    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }
        SWSS_LOG_DEBUG("processing range:%d-%d", range_low, range_high);
        for (size_t ind = range_low; ind <= range_high; ind++)
        {
            queue_ind = ind;
            SWSS_LOG_DEBUG("processing queue:%d", queue_ind);
            sai_object_id_t sai_scheduler_profile;
            resolve_result = resolveFieldRefValue(m_qos_type_maps, scheduler_field_name, tuple, sai_scheduler_profile);
            if (ref_resolve_status::success == resolve_result)
            {
                if (op == SET_COMMAND)
                {
                    result = applySchedulerToQueueSchedulerGroup(port, queue_ind, sai_scheduler_profile);
                }
                else if (op == DEL_COMMAND)
                {
                    // NOTE: The map is un-bound from the port. But the map itself still exists.
                    result = applySchedulerToQueueSchedulerGroup(port, queue_ind, SAI_NULL_OBJECT_ID);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                    return task_process_status::task_invalid_entry;
                }
                if (!result)
                {
                    SWSS_LOG_ERROR("Failed setting field:%s to port:%s, queue:%d, line:%d\n", scheduler_field_name.c_str(), port.m_alias.c_str(), queue_ind, __LINE__);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Applied scheduler to port:%s", port_name.c_str());
            }
            else if (resolve_result != ref_resolve_status::field_not_found)
            {
                if(ref_resolve_status::not_resolved == resolve_result)
                {
                    SWSS_LOG_ERROR("Missing or invalid scheduler reference\n");
                    return task_process_status::task_need_retry;
                }
                SWSS_LOG_ERROR("Resolving scheduler reference failed\n");
                return task_process_status::task_failed;
            }

            sai_object_id_t sai_wred_profile;
            resolve_result = resolveFieldRefValue(m_qos_type_maps, wred_profile_field_name, tuple, sai_wred_profile);
            if (ref_resolve_status::success == resolve_result)
            {
                if (op == SET_COMMAND)
                {
                    result = applyWredProfileToQueue(port, queue_ind, sai_wred_profile);
                }
                else if (op == DEL_COMMAND)
                {
                    // NOTE: The map is un-bound from the port. But the map itself still exists.
                    result = applyWredProfileToQueue(port, queue_ind, SAI_NULL_OBJECT_ID);
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                    return task_process_status::task_invalid_entry;
                }
                if (!result)
                {
                    SWSS_LOG_ERROR("Failed setting field:%s to port:%s, queue:%d, line:%d\n", wred_profile_field_name.c_str(), port.m_alias.c_str(), queue_ind, __LINE__);
                    return task_process_status::task_failed;
                }
                SWSS_LOG_DEBUG("Applied wred profile to port:%s", port_name.c_str());
            }
            else if (resolve_result != ref_resolve_status::field_not_found)
            {
                if(ref_resolve_status::not_resolved == resolve_result)
                {
                    SWSS_LOG_ERROR("Missing or invalid wred reference\n");
                    return task_process_status::task_need_retry;
                }
                SWSS_LOG_ERROR("Resolving wred reference failed\n");
                return task_process_status::task_failed;
            }
        }
    }
    SWSS_LOG_DEBUG("finished");
    return task_process_status::task_success;
}

bool QosOrch::applyMapToPort(Port &port, sai_attr_id_t attr_id, sai_object_id_t map)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t    sai_status;
    attr.id = attr_id;
    attr.value.oid = map;
    if (SAI_STATUS_SUCCESS != (sai_status = sai_port_api->set_port_attribute(port.m_port_id, &attr)))
    {
        SWSS_LOG_ERROR("Failed setting sai object:%llx for port:%s, status:%s\n", map, port.m_alias.c_str(), sai_status);
        return false;
    }
    return true;
}

task_process_status QosOrch::ResolveMapAndApplyToPort(
    Port                    &port,
    sai_port_attr_t         port_attr,
    string                  field_name,
    KeyOpFieldsValuesTuple  &tuple,
    string                  op)
{
    SWSS_LOG_ENTER();
    sai_object_id_t sai_object = SAI_NULL_OBJECT_ID;
    bool result;
    ref_resolve_status resolve_result = resolveFieldRefValue(m_qos_type_maps, field_name, tuple, sai_object);
    if (ref_resolve_status::success == resolve_result)
    {
        if (op == SET_COMMAND)
        {
            result = applyMapToPort(port, port_attr, sai_object);
        }
        else if (op == DEL_COMMAND)
        {
            // NOTE: The map is un-bound from the port. But the map itself still exists.
            result = applyMapToPort(port, port_attr, SAI_NULL_OBJECT_ID);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            return task_process_status::task_invalid_entry;
        }
        if (!result)
        {
            SWSS_LOG_ERROR("Failed setting field:%s to port:%s, line:%d\n", field_name.c_str(), port.m_alias.c_str(), __LINE__);
            return task_process_status::task_failed;
        }
        SWSS_LOG_DEBUG("Applied field:%s to port:%s, line:%d\n", field_name.c_str(), port.m_alias.c_str(), __LINE__);
        return task_process_status::task_success;
    }
    else if (resolve_result != ref_resolve_status::field_not_found)
    {
        if(ref_resolve_status::not_resolved == resolve_result)
        {
            SWSS_LOG_ERROR("Missing or invalid %s reference\n", field_name.c_str());
            return task_process_status::task_need_retry;
        }
        SWSS_LOG_ERROR("Resolving %s reference failed\n", field_name.c_str());
        return task_process_status::task_failed;
    }
    return task_process_status::task_success;
}

task_process_status QosOrch::handlePortQosMapTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    KeyOpFieldsValuesTuple tuple = it->second;
    Port port;
    string key = kfvKey(tuple);
    string op = kfvOp(tuple);
    vector<string> port_names;
    sai_uint8_t pfc_enable = 0;
    bool pfc_enable_present = false;

    for (auto i = kfvFieldsValues(tuple).begin(); i != kfvFieldsValues(tuple).end(); i++)
    {
        if (fvField(*i) == pfc_enable_name)
        {
            pfc_enable_present = true;
            vector<string> queue_indexes;
            queue_indexes = tokenize(fvValue(*i), list_item_delimiter);
            for(string q_ind : queue_indexes)
            {
                sai_uint8_t q_val = stoi(q_ind);
                pfc_enable |= (1 << q_val);
            }
            break;
        }
    }

    port_names = tokenize(key, list_item_delimiter);

    for (string port_name : port_names)
    {
        Port port;
        SWSS_LOG_DEBUG("processing port:%s", port_name.c_str());
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port with alias:%s not found\n", port_name.c_str());
            return task_process_status::task_invalid_entry;
        }

        task_process_status task_status = ResolveMapAndApplyToPort(port, SAI_PORT_ATTR_QOS_DSCP_TO_TC_MAP, dscp_to_tc_field_name, tuple, op);
        if(task_process_status::task_failed == task_status)
        {
            return task_status;
        }
        task_status = ResolveMapAndApplyToPort(port, SAI_PORT_ATTR_QOS_TC_TO_QUEUE_MAP, tc_to_queue_field_name, tuple, op);
        if(task_process_status::task_failed == task_status)
        {
            return task_status;
        }
        task_status = ResolveMapAndApplyToPort(port, SAI_PORT_ATTR_QOS_TC_TO_PRIORITY_GROUP_MAP, tc_to_pg_map_field_name, tuple, op);
        if(task_process_status::task_failed == task_status)
        {
            return task_status;
        }
        task_status = ResolveMapAndApplyToPort(port, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP, pfc_to_pg_map_name, tuple, op);
        if(task_process_status::task_failed == task_status)
        {
            return task_status;
        }
        task_status = ResolveMapAndApplyToPort(port, SAI_PORT_ATTR_QOS_PFC_PRIORITY_TO_QUEUE_MAP, pfc_to_queue_map_name, tuple, op);
        if(task_process_status::task_failed == task_status)
        {
            return task_status;
        }
        if(pfc_enable_present)
        {
            sai_attribute_t pfc_attr;
            pfc_attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
            pfc_attr.value.u8 = pfc_enable;
            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &pfc_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Applying pfc_enable bits:0x%x to Port with alias:%s failed\n", pfc_enable, port_name.c_str());
                return task_process_status::task_failed;
            }

        }
    }
    return task_process_status::task_success;
}

void QosOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        //
        // make sure table is recognized, and we have handler for it
        //
        string qos_map_type_name = consumer.m_consumer->getTableName();
        if (m_qos_type_maps.find(qos_map_type_name) == m_qos_type_maps.end())
        {
            SWSS_LOG_ERROR("Unrecognised qos table encountered:%s\n", qos_map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        if (m_qos_handler_map.find(qos_map_type_name) == m_qos_handler_map.end())
        {
            SWSS_LOG_ERROR("No handler for key:%s found.\n", qos_map_type_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        task_process_status task_status = (this->*(m_qos_handler_map[qos_map_type_name]))(consumer);
        switch(task_status)
        {
            case task_process_status::task_success :
                dumpTuple(consumer, tuple);
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_DEBUG("Successfully processed item, removing from queue.");
                break;
            case task_process_status::task_invalid_entry :
                SWSS_LOG_ERROR("Invalid QOS task item was encountered, removing from queue.");
                dumpTuple(consumer, tuple);
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_failed :
                SWSS_LOG_ERROR("Processing QOS task item failed, exiting.");
                dumpTuple(consumer, tuple);
                return;
            case task_process_status::task_need_retry :
                SWSS_LOG_ERROR("Processing QOS task item failed, will retry.");
                dumpTuple(consumer, tuple);
                it++;
                break;
            default:
                SWSS_LOG_ERROR("Invdalid resolve result.");
                break;
        }
    }
}